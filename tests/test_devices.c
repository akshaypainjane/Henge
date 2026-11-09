#include "harness.h"

#include <fcntl.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "pvg_lite.h"
#include "sysreg.h"
#include "vmapple.h"

DECLARE_PROGRAM(p_timer_irq);
DECLARE_PROGRAM(p_uart);
extern const char devices_programs_begin[], devices_programs_finish[];

#define RAM_SIZE (288ull << 20) /* Room for the 272 MiB BDIF boot asset fixture. */

static HvMachine m;
static HvCpu cpu;
static HvVmapple board;
static int checks, failures;

static void check(const char *what, uint64_t got, uint64_t want) {
    checks++;
    if (got != want) {
        printf("  FAIL %s: %llx, expected %llx\n", what, (unsigned long long)got, (unsigned long long)want);
        failures++;
    }
}

static uint32_t rd32(uint64_t pa) {
    uint32_t v = 0;
    hv_phys_read(&m, pa, &v, 4);
    return v;
}
static uint8_t rd8(uint64_t pa) {
    uint8_t v = 0;
    hv_phys_read(&m, pa, &v, 1);
    return v;
}
static void wr32(uint64_t pa, uint32_t v) { hv_phys_write(&m, pa, &v, 4); }
static void wr16(uint64_t pa, uint16_t v) { hv_phys_write(&m, pa, &v, 2); }
static void wr8(uint64_t pa, uint8_t v) { hv_phys_write(&m, pa, &v, 1); }
static void wr64(uint64_t pa, uint64_t v) { hv_phys_write(&m, pa, &v, 8); }
static uint8_t *ram(uint64_t pa) { return hv_phys_ptr(&m, pa, 1); }

#define ICC(op0, op1, crn, crm, op2) ((op0) << 14 | (op1) << 11 | (crn) << 7 | (crm) << 3 | (op2))
#define ICC_IAR1 ICC(3, 0, 12, 12, 0)
#define ICC_EOIR1 ICC(3, 0, 12, 12, 1)
#define ICC_PMR ICC(3, 0, 4, 6, 0)
#define ICC_IGRPEN1 ICC(3, 0, 12, 12, 7)
#define ICC_SGI1R ICC(3, 0, 12, 11, 5)

static void test_camera(void) {
    static uint8_t y[HV_UVC_WIDTH * HV_UVC_HEIGHT], uv[sizeof y / 2];
    uint8_t buffer[HV_UVC_PAYLOAD];
    HvXhci *usb = hv_xhci_create_camera(&m, board.pcie);
    check("camera controller available", usb != NULL, 1);
    if (!usb) return;
    HvUvc *camera = usb->camera;
    check("camera port disconnected by default", usb->portsc[0] & 1, 0);
    check("camera ignores frames until enabled", hv_uvc_submit(camera, y, HV_UVC_WIDTH, uv, HV_UVC_WIDTH), 0);
    hv_xhci_camera_share(usb, true);
    hv_xhci_poll(usb);
    check("camera port connects when enabled", usb->portsc[0] & 1, 1);
    check("camera port is high speed", (usb->portsc[0] >> 10) & 15, 3);

    uint8_t request[8] = {0x80, 6, 0, 2, 0, 0, 255, 0};
    int size = hv_uvc_control(camera, request, buffer, 255);
    check("camera configuration descriptor size", size, 159);
    if (size != 159) return;
    check("camera advertises total descriptor size", buffer[2] | buffer[3] << 8, size);
    int offset = 0;
    while (offset < size && buffer[offset] >= 2) offset += buffer[offset];
    check("camera descriptor boundaries", offset, size);

    memset(y, 90, sizeof y);
    memset(uv, 128, sizeof uv);
    uv[0] = 119;
    check("camera accepts the current frame", hv_uvc_submit(camera, y, HV_UVC_WIDTH, uv, HV_UVC_WIDTH), 1);
    check("guest needs to select a streaming mode", hv_uvc_payload(camera, buffer, sizeof buffer), 0);
    memcpy(request, (uint8_t[]){0xa1, 0x81, 0, 1, 1, 0, 26, 0}, 8);
    check("guest gets UVC probe", hv_uvc_control(camera, request, buffer, 26), 26);
    uint32_t interval;
    memcpy(&interval, buffer + 4, sizeof interval);
    check("camera offers 30 fps to the guest", interval, 333333);
    memcpy(request, (uint8_t[]){0x21, 1, 0, 2, 1, 0, 26, 0}, 8);
    check("guest commits the supported camera mode", hv_uvc_control(camera, request, buffer, 26), 0);
    size_t bytes = hv_uvc_payload(camera, buffer, 512);
    check("camera starts the payload in a smaller USB read", bytes, 512);
    check("camera starts with the native NV12 luma plane", buffer[2] | buffer[3] << 8 | buffer[4] << 16 | (uint32_t)buffer[5] << 24, 0x5a5a5a5a);
    size_t frame = bytes > 2 ? bytes - 2 : 0;
    uint8_t fid = buffer[1] & 1;
    bytes = hv_uvc_payload(camera, buffer, HV_UVC_PAYLOAD - 512);
    check("camera continues the split USB payload", bytes, HV_UVC_PAYLOAD - 512);
    check("continuation starts with pixels instead of a second header", buffer[0], 90);
    frame += bytes;
    uint32_t first_chroma = 0;
    do {
        bytes = hv_uvc_payload(camera, buffer, sizeof buffer);
        if (!bytes) break;
        if (frame <= sizeof y && frame + bytes - 2 >= sizeof y + 2) {
            size_t chroma = 2 + sizeof y - frame;
            first_chroma = buffer[chroma] | (uint32_t)buffer[chroma + 1] << 8;
        }
        frame += bytes - 2;
    } while (!(buffer[1] & 2) && frame < sizeof y + sizeof uv);
    check("camera assembles a complete NV12 frame", frame, sizeof y + sizeof uv);
    check("native chroma plane follows luma with Cb and Cr in order", first_chroma, 0x8077);
    check("camera sets EOF and preserves the frame ID", buffer[1], 0x82 | fid);
    check("camera does not repeat an old frame", hv_uvc_payload(camera, buffer, sizeof buffer), 0);
    check("camera can queue another frame", hv_uvc_submit(camera, y, HV_UVC_WIDTH, uv, HV_UVC_WIDTH), 1);

    /* A posted bulk transfer can span two buffers and end with an Event Data TRB. */
    uint64_t ring = VMAPPLE_MEM + 0x60000, payload = ring + 0x1000, events = ring + 0x2000;
    uint32_t td[] = {
        (uint32_t)payload, (uint32_t)(payload >> 32), 64, 1u << 10 | 0x11,
        (uint32_t)(payload + 64), (uint32_t)(payload >> 32), 64, 1u << 10 | 0x11,
        0x12345678, 0, 0, 7u << 10 | 0x21,
    };
    hv_phys_write(&m, ring, td, sizeof td);
    wr32(ring + sizeof td + 12, 0);
    usb->device_slot = 1;
    usb->ep[3] = (HvXhciRing){.dequeue = ring, .ccs = 1};
    usb->ep_running[3] = 1;
    usb->intr[0].erstsz = 1;
    usb->intr[0].seg_base = events;
    usb->intr[0].seg_size = 16;
    hv_xhci_poll(usb);
    check("bulk TD gets video across both buffers", rd8(payload + 64), 90);
    check("bulk TD completes at the Event Data pointer", rd32(events), 0x12345678);
    check("bulk completion reports transferred length", rd32(events + 8), 1u << 24 | 128);
    check("bulk completion marks Event Data", rd32(events + 12) & 4, 4);

    uint64_t dcbaa = ring + 0x3000, context = dcbaa + 0x100, control = ring + 0x4000;
    uint64_t commands = ring + 0x5000, next_control = control + 0x100;
    usb->dcbaap = dcbaa;
    wr64(dcbaa + 8, context);
    wr32(context + 32, 1);
    usb->ep[1] = (HvXhciRing){.dequeue = control, .ccs = 1};
    uint32_t optional_descriptor[] = {0x21000680, 0x000a0409, 8, 2u << 10 | 0x41};
    hv_phys_write(&m, control, optional_descriptor, sizeof optional_descriptor);
    usb->pci.bar_write(&usb->pci, 0, 0x2004, 1, 4);
    check("unsupported camera request reports a stall", rd32(events + 16 + 8) >> 24, 6);
    check("camera control endpoint halts", rd32(context + 32) & 7, 2);
    check("halted camera exposes the failed TD pointer", rd32(context + 40), (uint32_t)control | 1);

    uint32_t resume[] = {
        0, 0, 0, 1u << 24 | 1u << 16 | 14u << 10 | 1,
        (uint32_t)next_control | 1, (uint32_t)(next_control >> 32), 0, 1u << 24 | 1u << 16 | 16u << 10 | 1,
        0, 0, 0, 0,
    };
    hv_phys_write(&m, commands, resume, sizeof resume);
    usb->cmd = (HvXhciRing){.dequeue = commands, .ccs = 1};
    usb->pci.bar_write(&usb->pci, 0, 0x2000, 0, 4);
    check("guest reset leaves camera control endpoint stopped", rd32(context + 32) & 7, 3);
    uint32_t get_probe[] = {
        0x010081a1, 26u << 16 | 1, 8, 2u << 10 | 0x41,
        (uint32_t)payload, (uint32_t)(payload >> 32), 26, 1u << 16 | 3u << 10 | 1,
        0, 0, 0, 4u << 10 | 0x21,
        0, 0, 0, 0,
    };
    hv_phys_write(&m, next_control, get_probe, sizeof get_probe);
    usb->pci.bar_write(&usb->pci, 0, 0x2004, 1, 4);
    check("camera probe works after the optional descriptor stalls", rd32(payload), 0x01010001);

    uint64_t short_ring = ring + 0x100, short_buffer = ring + 0x6000;
    uint32_t short_td[] = {
        (uint32_t)short_buffer, (uint32_t)(short_buffer >> 32), HV_UVC_PAYLOAD, 1u << 10 | 0x15,
        0x23456789, 0, 0, 7u << 10 | 0x21,
        0, 0, 0, 0,
    };
    hv_phys_write(&m, short_ring, short_td, sizeof short_td);
    usb->ep[3] = (HvXhciRing){.dequeue = short_ring, .ccs = 1};
    unsigned event_index = usb->intr[0].index;
    hv_xhci_poll(usb);
    check("short bulk TD produces only one completion", usb->intr[0].index, event_index + 1);
    check("short bulk TD points to the short transfer", rd32(events + event_index * 16), (uint32_t)short_ring);
    check("short bulk TD reports the remaining bytes", rd32(events + event_index * 16 + 8), 13u << 24 | 128);

    hv_xhci_camera_share(usb, false);
    hv_xhci_poll(usb);
    check("camera disconnects when sharing ends", usb->portsc[0] & 1, 0);
    check("no buffered frames after camera is off", hv_uvc_payload(camera, buffer, sizeof buffer), 0);
    hv_xhci_camera_share(usb, true);
    hv_xhci_poll(usb);
    check("camera reconnects after being enabled", usb->portsc[0] & 1, 1);
    check("reconnected camera has no stale frame", hv_uvc_payload(camera, buffer, sizeof buffer), 0);
    hv_xhci_camera_share(usb, false);
}

static void test_pvg_commands(void) {
    const uint64_t display = VMAPPLE_MEM + 0x20000;
    uint32_t setup[] = {1, 20, 0, 0, (uint32_t)(display >> 14)};
    hv_pvg_lite_command(board.pvg, setup, sizeof setup);

    uint32_t short_swap[] = {8, 28, 0, 0, 0, 42, 0};
    memset(ram(display + 0xf80), 0xff, 24);
    hv_pvg_lite_command(board.pvg, short_swap, sizeof short_swap);
    check("short GPU swap still presents", rd32(display + 0xfb0), 42);
    for (unsigned i = 0; i < 6; i++)
        check("short GPU swap zeroes missing gamma words", rd32(display + 0xf80 + 4 * i), 0);

    uint32_t full_swap[] = {8, 52, 0, 0, 0, 43, 0, 1, 2, 3, 4, 5, 6};
    hv_pvg_lite_command(board.pvg, full_swap, sizeof full_swap);
    check("full GPU swap presents", rd32(display + 0xfb0), 43);
    for (unsigned i = 0; i < 6; i++)
        check("full GPU swap preserves gamma words", rd32(display + 0xf80 + 4 * i), i + 1);

    short_swap[0] |= 0xffff0000u;
    hv_pvg_lite_command(board.pvg, short_swap, sizeof short_swap);
    check("truncated GPU barrier list does not present", rd32(display + 0xfb0), 43);

    const uint64_t root = VMAPPLE_MEM + 0x24000;
    wr32(VMAPPLE_GFX + 0x1030, (uint32_t)(root >> 14));
    wr32(VMAPPLE_GFX + 0x1010, 0x100);
    wr32(VMAPPLE_GFX + 0x1004, 0x120);
    wr32(root + 0x100, 0);
    wr32(root + 0x104, 16);
    wr32(root + 0x108, 0x1234);

    wr32(VMAPPLE_GFX + 0x1008, 1);
    check("invalid GPU root pointer does not complete a command", rd32(root), 0);
    wr32(VMAPPLE_GFX + 0x1008, 16);
    check("valid GPU root command completes", rd32(root), 0x1234);

    wr32(root + 0x110, 0);
    wr32(root + 0x114, 20);
    wr32(root + 0x118, 0x5678);
    wr32(VMAPPLE_GFX + 0x1008, 32);
    check("GPU root command cannot consume beyond submitted bytes", rd32(root), 0x1234);

    wr64(VMAPPLE_IOSFC + 0x1000, root);
    wr64(VMAPPLE_IOSFC + 0x1008, 1);
    wr64(VMAPPLE_IOSFC + 0x1018, UINT64_MAX);
    check("GPU mapper rejects a producer more than one ring ahead", rd32(VMAPPLE_IOSFC + 0x1020), 0);
    wr64(VMAPPLE_IOSFC + 0x1018, 1);
    check("GPU mapper accepts a valid producer after a rejected doorbell", rd32(VMAPPLE_IOSFC + 0x1020), 1);
}

static void test_gic(void) {
    HvGic *g = board.gic;
    check("GICD_TYPER ITLines", rd32(VMAPPLE_GIC_DIST + 4) & 0x1f, HV_GIC_NUM_IRQS / 32 - 1);
    check("GICD_PIDR2 arch rev", (rd32(VMAPPLE_GIC_DIST + 0xffe8) >> 4) & 0xf, 3);
    check("GICR_TYPER Last", (rd32(VMAPPLE_GIC_REDIST + 8) >> 4) & 1, 1);
    check("GICR_WAKER asleep at reset", rd32(VMAPPLE_GIC_REDIST + 0x14), 6);
    wr32(VMAPPLE_GIC_DIST, 3);
    wr32(VMAPPLE_GIC_REDIST + 0x14, 0);
    check("GICR_WAKER awake", rd32(VMAPPLE_GIC_REDIST + 0x14), 0);
    m.icc_write(&m, &cpu, ICC_PMR, 0xff);
    m.icc_write(&m, &cpu, ICC_IGRPEN1, 1);

    /* SPI 33 (UART), level, group 1, priority 0x80 */
    wr32(VMAPPLE_GIC_DIST + 0x84, 1u << 1);
    wr32(VMAPPLE_GIC_DIST + 0x104, 1u << 1);
    wr8(VMAPPLE_GIC_DIST + 0x400 + 33, 0x80);
    check("ISENABLER1 reads back", rd32(VMAPPLE_GIC_DIST + 0x104) & 2, 2);
    hv_gicv3_set_irq(g, 33, 1);
    check("level SPI raises IRQ", m.percpu[0].irq_line, 1);
    check("ISPENDR1 shows it", (rd32(VMAPPLE_GIC_DIST + 0x204) >> 1) & 1, 1);
    check("IAR1 = 33", m.icc_read(&m, &cpu, ICC_IAR1), 33);
    check("active: line drops", m.percpu[0].irq_line, 0);
    check("nothing else pending", m.icc_read(&m, &cpu, ICC_IAR1), 1023);
    m.icc_write(&m, &cpu, ICC_EOIR1, 33);
    check("still-high level re-pends after EOI", m.percpu[0].irq_line, 1);
    hv_gicv3_set_irq(g, 33, 0);
    check("level low: no IRQ", m.percpu[0].irq_line, 0);

    /* PMR masks; a higher-priority interrupt preempts a running one. */
    wr32(VMAPPLE_GIC_DIST + 0x84, 3u << 1);
    wr32(VMAPPLE_GIC_DIST + 0x104, 1u << 2);
    wr8(VMAPPLE_GIC_DIST + 0x400 + 34, 0x40);
    m.icc_write(&m, &cpu, ICC_PMR, 0x80);
    hv_gicv3_set_irq(g, 33, 1);
    check("priority 0x80 masked by PMR 0x80", m.percpu[0].irq_line, 0);
    m.icc_write(&m, &cpu, ICC_PMR, 0xff);
    check("unmasked", m.icc_read(&m, &cpu, ICC_IAR1), 33);
    hv_gicv3_set_irq(g, 34, 1);
    check("higher priority preempts", m.percpu[0].irq_line, 1);
    check("IAR1 = 34", m.icc_read(&m, &cpu, ICC_IAR1), 34);
    m.icc_write(&m, &cpu, ICC_EOIR1, 34);
    hv_gicv3_set_irq(g, 34, 0);
    m.icc_write(&m, &cpu, ICC_EOIR1, 33);
    hv_gicv3_set_irq(g, 33, 0);

    /* Edge SPI: one pulse, one interrupt. */
    wr32(VMAPPLE_GIC_DIST + 0xc08, 2u << 2);  /* ICFGR2: INTID 33 edge */
    hv_gicv3_set_irq(g, 33, 1);
    hv_gicv3_set_irq(g, 33, 0);
    check("edge latched", m.icc_read(&m, &cpu, ICC_IAR1), 33);
    m.icc_write(&m, &cpu, ICC_EOIR1, 33);
    check("edge consumed", m.percpu[0].irq_line, 0);
    wr32(VMAPPLE_GIC_DIST + 0xc08, 0);

    /* SGI 5 to ourselves (redistributor bank) */
    wr32(VMAPPLE_GIC_REDIST + 0x10000 + 0x80, 1u << 5);
    wr32(VMAPPLE_GIC_REDIST + 0x10000 + 0x100, 1u << 5);
    m.icc_write(&m, &cpu, ICC_SGI1R, 5ull << 24 | 1);
    check("SGI to self", m.icc_read(&m, &cpu, ICC_IAR1), 5);
    m.icc_write(&m, &cpu, ICC_EOIR1, 5);
    check("ICENABLER1 clears", (wr32(VMAPPLE_GIC_DIST + 0x184, 6), rd32(VMAPPLE_GIC_DIST + 0x104) & 6), 0);
}

static int make_disk(const char *path, uint64_t size, uint8_t salt) {
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    static uint8_t buf[4096];
    for (uint64_t off = 0; off < size; off += sizeof buf) {
        for (size_t i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)((off + i) / 512 * 7 + i + salt);
        if (write(fd, buf, sizeof buf) != (ssize_t)sizeof buf) return -1;
    }
    close(fd);
    return 0;
}

static void test_simple_devices(void) {
    /* cfg region */
    check("cfg version", rd32(VMAPPLE_CONFIG + 0), 2);
    check("cfg nr_cpus", rd32(VMAPPLE_CONFIG + 4), 1);
    uint64_t rs = 0;
    hv_phys_read(&m, VMAPPLE_CONFIG + 0x20, &rs, 8);
    check("cfg ram_size", rs, RAM_SIZE);
    char soc[20];
    for (int i = 0; i < 19; i++) soc[i] = (char)rd8(VMAPPLE_CONFIG + 0x584 + i);
    check("cfg soc_name", memcmp(soc, "Apple M1 (Virtual)", 19), 0);
    check("cfg mac_en0", rd8(VMAPPLE_CONFIG + 0x40 + 5), 0x56);
    uint32_t r1 = rd32(VMAPPLE_CONFIG + 0x38), r2 = rd32(VMAPPLE_CONFIG + 0x38);
    check("cfg rnd changes on every read", r1 != r2, 1);
    wr32(VMAPPLE_CONFIG + 0x3ac, 0x290001);
    check("cfg scratch is writable", rd32(VMAPPLE_CONFIG + 0x3ac), 0x290001);

    /* PL031 */
    uint32_t t = rd32(VMAPPLE_RTC);
    check("RTC within 2 s of host time", (uint64_t)((int64_t)t - (int64_t)time(NULL) + 2) <= 4, 1);
    wr32(VMAPPLE_RTC + 8, 1000000);
    check("RTC load", rd32(VMAPPLE_RTC) - 1000000 <= 2, 1);
    check("RTC PeriphID0", rd32(VMAPPLE_RTC + 0xfe0), 0x31);
    wr32(VMAPPLE_RTC + 0x10, 1);
    wr32(VMAPPLE_RTC + 4, rd32(VMAPPLE_RTC)); /* match now */
    check("RTC alarm on SPI 2", board.gic->level[32 + VMAPPLE_IRQ_RTC], 1);
    wr32(VMAPPLE_RTC + 0x1c, 1);
    check("RTC alarm cleared", board.gic->level[32 + VMAPPLE_IRQ_RTC], 0);

    /* PL011 receive side */
    wr32(VMAPPLE_UART + 0x38, 1u << 4);  /* IMSC: RX */
    check("UART RX empty", (rd32(VMAPPLE_UART + 0x18) >> 4) & 1, 1);
    hv_pl011_receive(board.uart, 'Z');
    check("UART RX line", board.gic->level[32 + VMAPPLE_IRQ_UART], 1);
    check("UART DR", rd32(VMAPPLE_UART), 'Z');
    check("UART RX line drops", board.gic->level[32 + VMAPPLE_IRQ_UART], 0);
    wr32(VMAPPLE_UART + 0x38, 0);
}

static void test_bdif(HvDisk *root, HvDisk *aux) {
    /* request at RAM+0x1000: sector cmd, data, retval */
    uint64_t base = VMAPPLE_MEM;
    uint64_t req = base + 0x1000, sec = base + 0x2000, data = base + 0x3000, ret = base + 0x4000;
    uint32_t sector[4] = {0, 0, 3, 0};
    memcpy(ram(sec), sector, 16);
    uint8_t r[48] = {0};
    uint64_t a;
    uint32_t l, f;
    a = sec; l = 16; f = 0; memcpy(r, &a, 8); memcpy(r + 8, &l, 4); memcpy(r + 12, &f, 4);
    a = data; l = 1024; f = 0x00030001; memcpy(r + 16, &a, 8); memcpy(r + 24, &l, 4); memcpy(r + 28, &f, 4);
    a = ret; l = 1; f = 0; memcpy(r + 32, &a, 8); memcpy(r + 40, &l, 4); memcpy(r + 44, &f, 4);
    memcpy(ram(req), r, 48);
    check("bdif status", rd32(VMAPPLE_BDOOR), 1);
    check("bdif root capacity", rd32(VMAPPLE_BDOOR + 0x420), root->size / 512);
    check("bdif aux capacity", rd32(VMAPPLE_BDOOR + 0x10000 + 0x420), aux->size / 512);
    uint64_t original_size = root->size;
    root->size = 96ull << 30;
    check("bdif expanded root capacity", rd32(VMAPPLE_BDOOR + 0x420), (96ull << 30) / 512);
    root->size = original_size;
    for (int which = 0; which < 2; which++) {
        HvDisk *d = which ? aux : root;
        ram(ret)[0] = 0xee;
        wr64(VMAPPLE_BDOOR + (which ? 0x10000 : 0) + 0x408, req);
        static uint8_t want[1024];
        hv_disk_read(d, 3 * 512, want, 1024);
        check(which ? "bdif aux read ok" : "bdif root read ok", ram(ret)[0], 0);
        check(which ? "bdif aux data" : "bdif root data", memcmp(ram(data), want, 1024), 0);
    }

    /* The 27.3 upgrade firmware reads this asset in one request. */
    const uint32_t large_length = 272u << 20;
    const uint64_t disk_offset = 4ull << 20;
    const uint64_t large_data = base + (1ull << 20);
    const uint8_t marker = 0xa7;
    int resized = ftruncate(root->fd, (off_t)(disk_offset + large_length));
    check("large boot disk fixture", resized, 0);
    if (resized != 0) return;
    root->size = disk_offset + large_length;
    check("large boot asset first marker", pwrite(root->fd, &marker, 1, (off_t)disk_offset), 1);
    check("large boot asset last marker", pwrite(root->fd, &marker, 1, (off_t)(root->size - 1)), 1);
    sector[2] = (uint32_t)(disk_offset / 512);
    memcpy(ram(sec), sector, sizeof sector);
    memcpy(r + 16, &large_data, sizeof large_data);
    memcpy(r + 24, &large_length, sizeof large_length);
    memcpy(ram(req), r, sizeof r);
    ram(ret)[0] = 0xee;
    wr64(VMAPPLE_BDOOR + 0x408, req);
    check("bdif large boot asset read", ram(ret)[0], 0);
    check("bdif large read first byte", ram(large_data)[0], marker);
    check("bdif large read last byte", ram(large_data)[large_length - 1], marker);

    uint64_t invalid_buffer = base + RAM_SIZE - 1024;
    memcpy(ram(req) + 16, &invalid_buffer, sizeof invalid_buffer);
    wr64(VMAPPLE_BDOOR + 0x408, req);
    check("bdif rejects large read outside guest RAM", ram(ret)[0], 1);

    memcpy(ram(req) + 16, &large_data, sizeof large_data);
    sector[2]++;
    memcpy(ram(sec), sector, sizeof sector);
    ram(ret)[0] = 0xee;
    wr64(VMAPPLE_BDOOR + 0x408, req);
    check("bdif rejects large read outside disk", ram(ret)[0], 1);
    check("restore boot disk fixture", ftruncate(root->fd, (off_t)original_size), 0);
    root->size = original_size;
}

/* A small virtio driver, as a guest would do it. */
static uint64_t find_slot(uint16_t vendor, uint16_t device, int nth) {
    for (int s = 0; s < 32; s++) {
        uint32_t id = rd32(VMAPPLE_PCIE_ECAM + ((uint64_t)s << 15));
        if (id == ((uint32_t)device << 16 | vendor) && nth-- == 0) return (uint64_t)s;
    }
    return ~0ull;
}

static uint64_t setup_virtio(int slot, uint64_t bar, uint64_t ring) {
    uint64_t cfg = VMAPPLE_PCIE_ECAM + ((uint64_t)slot << 15);
    wr32(cfg + 0x20, 0xffffffff);
    check("BAR4 size 16 KB, 64-bit", rd32(cfg + 0x20), 0xffffc004);
    wr32(cfg + 0x20, (uint32_t)bar);
    wr32(cfg + 0x24, 0);
    wr16(cfg + 0x04, 0x6); /* memory, bus master */
    /* walk the capabilities to the common config */
    uint8_t cap = rd8(cfg + 0x34);
    int found = 0;
    while (cap) {
        if (rd8(cfg + cap) == 0x09 && rd8(cfg + cap + 3) == 1 && rd8(cfg + cap + 4) == 4) found = 1;
        cap = rd8(cfg + cap + 1);
    }
    check("virtio common cfg capability", found, 1);
    wr8(bar + 0x14, 0);
    wr8(bar + 0x14, 1 | 2); /* ACKNOWLEDGE, DRIVER */
    wr32(bar + 0x00, 1);
    check("VERSION_1 offered", rd32(bar + 0x04) & 1, 1);
    wr32(bar + 0x08, 1);
    wr32(bar + 0x0c, 1);
    wr8(bar + 0x14, 1 | 2 | 8);
    check("FEATURES_OK", rd8(bar + 0x14) & 8, 8);
    for (int q = 0; q < 2; q++) {
        uint64_t qr = ring + (uint64_t)q * 0x4000;
        memset(ram(qr), 0, 0x4000);
        wr16(bar + 0x16, (uint16_t)q);
        wr16(bar + 0x18, 16);
        wr64(bar + 0x20, qr);
        wr64(bar + 0x28, qr + 0x1000);
        wr64(bar + 0x30, qr + 0x2000);
        wr16(bar + 0x1c, 1);
    }
    wr8(bar + 0x14, 1 | 2 | 8 | 4);
    return ring;
}

static void put_desc(uint64_t table, int i, uint64_t addr, uint32_t len, uint16_t flags, uint16_t next) {
    uint8_t *d = ram(table + 16u * i);
    memcpy(d, &addr, 8);
    memcpy(d + 8, &len, 4);
    memcpy(d + 12, &flags, 2);
    memcpy(d + 14, &next, 2);
}

static void offer(uint64_t ring, uint16_t head) {
    uint16_t *avail = (uint16_t *)ram(ring + 0x1000);
    avail[2 + avail[1] % 16] = head;
    avail[1]++;
}

static uint16_t used_idx(uint64_t ring) {
    return ((uint16_t *)ram(ring + 0x2000))[1];
}

static void test_virtio_blk(HvDisk *aux) {
    int slot = (int)find_slot(0x106b, 0x1a00, 0);
    check("Apple virtio-blk (aux) enumerated", slot != (int)~0u, 1);
    if (slot < 0 || slot >= 32) return;
    check("class: SCSI storage", rd32(VMAPPLE_PCIE_ECAM + ((uint64_t)slot << 15) + 8) >> 8, 0x010000);
    /* Walk the capabilities as AppleVirtIO's transport does: vendor data, cfg_type 9, vendor 0x106b. */
    {
        uint64_t c = VMAPPLE_PCIE_ECAM + ((uint64_t)slot << 15);
        int found = 0;
        for (uint8_t at = rd8(c + 0x34); at; at = rd8(c + at + 1)) {
            if (rd8(c + at) != 0x09 || rd8(c + at + 3) != 9) continue;
            uint16_t vend = (uint16_t)(rd8(c + at + 4) | rd8(c + at + 5) << 8);
            if (vend == 0x106b && rd8(c + at + 2) >= 6 + 8 && (rd8(c + at + 6) & 1)) found = 1;
        }
        check("Apple vendor data says: not inline-encrypted", found, 1);
    }
    uint64_t bar = VMAPPLE_PCIE_MMIO + 0x100000;
    uint64_t ring = setup_virtio(slot, bar, VMAPPLE_MEM + 0x100000);
    check("blk capacity", rd32(bar + 0x2000), (uint32_t)(aux->size / 512));
    check("apple type in max_secure_erase_sectors", rd32(bar + 0x2000 + 60), HV_VBLK_AUX);

    uint64_t hdr = VMAPPLE_MEM + 0x200000, data = hdr + 0x1000, status = hdr + 0x3000;
    /* read sectors 5-6 */
    uint32_t h[4] = {0, 0, 5, 0};
    memcpy(ram(hdr), h, 16);
    put_desc(ring, 0, hdr, 16, 1, 1);
    put_desc(ring, 1, data, 1024, 1 | 2, 2);
    put_desc(ring, 2, status, 1, 2, 0);
    ram(status)[0] = 0xff;
    offer(ring, 0);
    wr16(bar + 0x3000, 0);
    hv_virtio_blk_drain(board.aux_blk); /* requests complete on the worker thread */
    static uint8_t want[1024];
    hv_disk_read(aux, 5 * 512, want, 1024);
    check("blk read completed", used_idx(ring), 1);
    check("blk read status", ram(status)[0], 0);
    check("blk read data", memcmp(ram(data), want, 1024), 0);
    int line = 32 + VMAPPLE_IRQ_PCIE + slot;
    check("INTx raised", board.gic->level[line], 1);
    check("ISR", rd8(bar + 0x1000), 1);
    check("ISR read drops INTx", board.gic->level[line], 0);

    /* write sector 9, then read it back from the file */
    h[0] = 1;
    h[2] = 9;
    memcpy(ram(hdr), h, 16);
    for (int i = 0; i < 512; i++) ram(data)[i] = (uint8_t)(i ^ 0x5a);
    put_desc(ring, 3, hdr, 16, 1, 4);
    put_desc(ring, 4, data, 512, 1, 5);
    put_desc(ring, 5, status, 1, 2, 0);
    offer(ring, 3);
    wr16(bar + 0x3000, 0);
    hv_virtio_blk_drain(board.aux_blk); /* requests complete on the worker thread */
    uint8_t back[512];
    hv_disk_read(aux, 9 * 512, back, 512);
    check("blk write status", ram(status)[0], 0);
    check("blk write reached the file", memcmp(back, ram(data), 512), 0);

    h[0] = 4; /* VIRTIO_BLK_T_FLUSH */
    memcpy(ram(hdr), h, 16);
    put_desc(ring, 6, hdr, 16, 1, 7);
    put_desc(ring, 7, status, 1, 2, 0);
    const HvDiskFlushMode modes[] = {HV_DISK_FLUSH_SYNC, HV_DISK_FLUSH_BARRIER};
    for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++) {
        aux->flush_mode = modes[i];
        ram(status)[0] = 0xff;
        offer(ring, 6);
        wr16(bar + 0x3000, 0);
        hv_virtio_blk_drain(board.aux_blk);
        check("guest flush succeeds on the backing file", ram(status)[0], 0);
    }
    aux->flush_mode = HV_DISK_FLUSH_LEGACY;

    /* Apple barrier, then an unknown type */
    h[0] = 0x10000;
    memcpy(ram(hdr), h, 16);
    offer(ring, 6);
    wr16(bar + 0x3000, 0);
    hv_virtio_blk_drain(board.aux_blk); /* requests complete on the worker thread */
    check("Apple barrier OK", ram(status)[0], 0);
    h[0] = 77;
    memcpy(ram(hdr), h, 16);
    offer(ring, 6);
    wr16(bar + 0x3000, 0);
    hv_virtio_blk_drain(board.aux_blk); /* requests complete on the worker thread */
    check("unknown request UNSUPP", ram(status)[0], 2);
    check("six requests used", used_idx(ring), 6);
    rd8(bar + 0x1000);
}

static uint8_t sent[2048];
static uint32_t sent_len;
static void capture_tx(void *opaque, const uint8_t *frame, uint32_t len) {
    (void)opaque;
    memcpy(sent, frame, len < sizeof sent ? len : sizeof sent);
    sent_len = len;
}

static void test_virtio_net(void) {
    int slot = (int)find_slot(0x1af4, 0x1041, 0);
    check("virtio-net enumerated", slot >= 0 && slot < 32, 1);
    if (slot < 0 || slot >= 32) return;
    uint64_t bar = VMAPPLE_PCIE_MMIO + 0x200000;
    uint64_t ring = setup_virtio(slot, bar, VMAPPLE_MEM + 0x300000);
    check("net MAC in config", rd8(bar + 0x2000 + 5), 0x56);
    board.net->transmit = capture_tx;
    uint64_t rxq = ring, txq = ring + 0x4000, buf = VMAPPLE_MEM + 0x400000;
    /* transmit: 12-byte header + 60-byte frame */
    memset(ram(buf), 0, 12);
    for (int i = 0; i < 60; i++) ram(buf + 12)[i] = (uint8_t)i;
    put_desc(txq, 0, buf, 72, 0, 0);
    offer(txq, 0);
    wr16(bar + 0x3000 + 4, 1);
    check("TX frame length", sent_len, 60);
    check("TX frame bytes", sent[59], 59);
    /* receive */
    uint8_t frame[64];
    for (int i = 0; i < 64; i++) frame[i] = (uint8_t)(0xa0 + i);
    check("RX with no buffer drops", hv_virtio_net_receive(board.net, frame, 64), 0);
    put_desc(rxq, 0, buf + 0x1000, 2048, 2, 0);
    offer(rxq, 0);
    wr16(bar + 0x3000, 0);
    check("RX delivered", hv_virtio_net_receive(board.net, frame, 64), 1);
    check("RX bytes", memcmp(ram(buf + 0x1000 + 12), frame, 64), 0);
    uint32_t used_len;
    memcpy(&used_len, ram(rxq + 0x2000 + 8), 4);
    check("RX used length", used_len, 76);
}

/* FIPS-197 appendix C.1: AES-128, key 000102..0f, plaintext 00112233..ff. */
static void test_aes(void) {
    const uint64_t fifo = VMAPPLE_AES_1 + 0x200, src = VMAPPLE_MEM + 0x500000, dst = src + 0x100;
    static const uint8_t pt[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                   0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    static const uint8_t ct[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
                                   0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
    memcpy(ram(src), pt, 16);
    check("aes status at reset", rd32(VMAPPLE_AES_1 + 0x0c), 0x3f80);
    wr32(fifo, 1u << 28 | 1u << 20); /* KEY: context 0, AES-128, encrypt, ECB */
    for (int i = 0; i < 4; i++) wr32(fifo, 0x03020100u + 0x04040404u * (uint32_t)i);
    wr32(fifo, 5u << 28 | 16); /* DATA: 16 bytes */
    wr32(fifo, 0);
    wr32(fifo, (uint32_t)src);
    wr32(fifo, (uint32_t)dst);
    check("aes-128 ECB known answer", memcmp(ram(dst), ct, 16), 0);
    wr32(VMAPPLE_AES_1 + 0x1c, 1u << 5);
    wr32(fifo, 8u << 28 | 1u << 27 | 0x42); /* FLAG with interrupt */
    check("aes flag raises SPI 0x12", board.gic->level[32 + VMAPPLE_IRQ_AES], 1);
    check("aes flag info", rd32(VMAPPLE_AES_1 + 0x30), 0x42);
    wr32(VMAPPLE_AES_1 + 0x18, 1u << 5);
    check("aes interrupt cleared", board.gic->level[32 + VMAPPLE_IRQ_AES], 0);
}

static char uart_out[128];
static int uart_n;
static void capture_uart(void *opaque, uint8_t c) {
    (void)opaque;
    if (uart_n < (int)sizeof uart_out - 1) uart_out[uart_n++] = (char)c;
}

static void test_guest_programs(void) {
    static HvCpu init;
    hv_machine_add_ram(&m, (uint64_t)(uintptr_t)devices_programs_begin, devices_programs_finish - devices_programs_begin,
                       (void *)devices_programs_begin, 1);
    static const TestProgram timer = TEST_PROGRAM(p_timer_irq), uart = TEST_PROGRAM(p_uart);

    /* Fresh GIC state for the guest to configure. */
    HvGic *g = board.gic;
    memset(g->enabled, 0, sizeof g->enabled);
    memset(g->cpu[0].enabled, 0, sizeof g->cpu[0].enabled);
    g->ctlr = 0;
    g->cpu[0].igrpen1 = 0;
    g->cpu[0].waker = 6;
    memset(&init, 0, sizeof init);
    seed_cpu(&init, 3, VMAPPLE_MEM + 0x10000);
    uint64_t t0 = hv_counter();
    if (run_jit(&m, &timer, &init, &cpu, NULL) == 0) {
        check("timer IRQ INTID", cpu.x[20], 27);
        check("one IRQ", cpu.x[21], 1);
        check("waited about 1 ms (counter delta >= 24000)", cpu.x[11] - cpu.x[10] >= 24000, 1);
        check("not absurdly long (< 50 ms)", hv_counter() - t0 < 24000 * 50, 1);
        check("no stray exceptions", cpu.x[23], 0);
        check("RPR idle after EOI", cpu.x[12], 0xff);
    } else {
        failures++;
    }

    board.uart->output = capture_uart;
    memset(&init, 0, sizeof init);
    seed_cpu(&init, 4, VMAPPLE_MEM + 0x10000);
    if (run_jit(&m, &uart, &init, &cpu, NULL) == 0) {
        check("UART output", strcmp(uart_out, "hello from the guest\n"), 0);
        check("PL011 PeriphID0", cpu.x[4], 0x11);
    } else {
        failures++;
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (hv_machine_init(&m, 4 << 20) != 0) return 1;
    char work[] = "./henge-test-devices.XXXXXX";
    char root_path[sizeof work + sizeof "/test-root.img"];
    char aux_path[sizeof work + sizeof "/test-aux.img"];
    if (!mkdtemp(work)) return 1;
    snprintf(root_path, sizeof root_path, "%s/test-root.img", work);
    snprintf(aux_path, sizeof aux_path, "%s/test-aux.img", work);
    static HvDisk root, aux;
    /* The auxiliary disk skips a 16 KB header, as with auxiliary.bin. */
    if (make_disk(root_path, 1 << 20, 1) || make_disk(aux_path, 256 << 10, 2) ||
        hv_disk_open(&root, root_path, 0, 0, true) != 0 ||
        hv_disk_open(&aux, aux_path, 16384, 0, false) != 0) {
        printf("cannot prepare test disks in %s\n", work);
        unlink(root_path);
        unlink(aux_path);
        rmdir(work);
        return 1;
    }
    HvCfgParams cfg = {.nr_cpus = 1, .ecid = 0x1234, .mac_en0 = {2, 0, 0, 0x12, 0x34, 0x56}};
    static const uint8_t mac[6] = {2, 0, 0, 0x12, 0x34, 0x56};
    if (hv_vmapple_create(&board, &m, &cpu, RAM_SIZE, &root, &aux, &cfg, mac) != 0) return 1;
    hv_vmapple_reset_cpu(&board);
    check("reset pc", cpu.pc, VMAPPLE_FIRMWARE);
    check("aux disk skips its 16 KB header", aux.size, (256 << 10) - 16384);

    test_gic();
    test_simple_devices();
    test_bdif(&root, &aux);
    test_virtio_blk(&aux);
    test_virtio_net();
    test_aes();
    /* MSI frame: valid type register, doorbell pends the INTID */
    check("msi type valid", rd32(VMAPPLE_GIC_MSI + 8) >> 31, 1);
    check("msi base", (rd32(VMAPPLE_GIC_MSI + 8) >> 16) & 0x1fff, VMAPPLE_MSI_BASE);
    wr32(VMAPPLE_GIC_MSI + 0x40, VMAPPLE_MSI_BASE + 3);
    check("msi doorbell pends the INTID", board.gic->latch[VMAPPLE_MSI_BASE + 3], 1);
    /* PL061: power button on pin 3, rising edge */
    wr32(VMAPPLE_GPIO + 0x40c, 1u << 3); /* IEV: rising edge */
    wr32(VMAPPLE_GPIO + 0x410, 1u << 3);
    check("gpio PeriphID0", rd32(VMAPPLE_GPIO + 0xfe0), 0x61);
    hv_pl061_set_input(board.gpio, 3, 1);
    check("gpio pin 3 reads high", rd32(VMAPPLE_GPIO + (1u << 3 << 2)), 1u << 3);
    check("gpio edge raises SPI 5", board.gic->level[32 + VMAPPLE_IRQ_GPIO], 1);
    wr32(VMAPPLE_GPIO + 0x41c, 1u << 3);
    check("gpio interrupt cleared", board.gic->level[32 + VMAPPLE_IRQ_GPIO], 0);
    test_guest_programs();
    test_pvg_commands();
    test_camera();

    hv_disk_close(&root);
    hv_disk_close(&aux);
    unlink(root_path);
    unlink(aux_path);
    rmdir(work);
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
