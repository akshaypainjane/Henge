#include "uvc.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    LUMA_BYTES = HV_UVC_WIDTH * HV_UVC_HEIGHT,
    FRAME_BYTES = LUMA_BYTES + LUMA_BYTES / 2,
    INTERVAL = (10000000 + HV_UVC_FPS / 2) / HV_UVC_FPS,
    PROBE_BYTES = 26,
};

#define LE16(x) (uint8_t)(x), (uint8_t)((x) >> 8)
#define LE32(x) LE16(x), LE16((x) >> 16)

static const uint8_t device[] = {
    18, 1, LE16(0x0200), 0xef, 2, 1, 64, LE16(0x1b36), LE16(0x0101),
    LE16(0x0100), 1, 2, 0, 1,
};
static const uint8_t qualifier[] = {10, 6, LE16(0x0200), 0xef, 2, 1, 64, 1, 0};
static const uint8_t config[] = {
    9, 2, LE16(159), 2, 1, 0, 0x80, 50,
    8, 11, 0, 2, 14, 3, 0, 0,              /* video interface association */
    9, 4, 0, 0, 0, 14, 1, 0, 0,            /* VideoControl interface */
    13, 0x24, 1, LE16(0x0100), LE16(40), LE32(48000000), 1, 1,
    18, 0x24, 2, 1, LE16(0x0201), 0, 0, LE16(0), LE16(0), LE16(0), 3, 0, 0, 0, /* camera terminal */
    9, 0x24, 3, 2, LE16(0x0101), 0, 1, 0,  /* USB streaming terminal */
    9, 4, 1, 0, 1, 14, 2, 0, 0,            /* VideoStreaming, bulk endpoint on alt 0 */
    14, 0x24, 1, 1, LE16(77), 0x81, 0, 2, 0, 0, 0, 1, 0,
    27, 0x24, 4, 1, 1, 'N', 'V', '1', '2', 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71,
    12, 1, 0, 0, 0, 0,
    30, 0x24, 5, 1, 0, LE16(HV_UVC_WIDTH), LE16(HV_UVC_HEIGHT),
    LE32(FRAME_BYTES * 8 * HV_UVC_FPS), LE32(FRAME_BYTES * 8 * HV_UVC_FPS),
    LE32(FRAME_BYTES), LE32(INTERVAL), 1, LE32(INTERVAL),
    6, 0x24, 0x0d, 1, 1, 4,               /* color matching; BT.601 matrix */
    7, 5, 0x81, 2, LE16(512), 0,
};
_Static_assert(sizeof config == 159, "UVC configuration length");
static const uint8_t probe[] = {
    LE16(1), 1, 1, LE32(INTERVAL), LE16(0), LE16(0), LE16(0), LE16(0), LE16(0),
    LE32(FRAME_BYTES), LE32(HV_UVC_PAYLOAD),
};
_Static_assert(sizeof probe == PROBE_BYTES, "UVC 1.0 probe length");

struct HvUvc {
    pthread_mutex_t lock;
    atomic_bool shared;
    bool committed, pending;
    uint8_t config, fid;
    size_t offset, payload_left;
    uint64_t submitted, delivered;
    uint8_t *latest, *current;
    uint8_t frames[2][FRAME_BYTES];
};

HvUvc *hv_uvc_create(void) {
    HvUvc *camera = calloc(1, sizeof *camera);
    if (!camera) return NULL;
    if (pthread_mutex_init(&camera->lock, NULL) != 0) {
        free(camera);
        return NULL;
    }
    camera->latest = camera->frames[0];
    camera->current = camera->frames[1];
    atomic_init(&camera->shared, false);
    return camera;
}

void hv_uvc_destroy(HvUvc *camera) {
    pthread_mutex_destroy(&camera->lock);
    free(camera);
}

void hv_uvc_reset(HvUvc *camera) {
    pthread_mutex_lock(&camera->lock);
    camera->config = 0;
    camera->committed = false;
    camera->offset = 0;
    camera->payload_left = 0;
    pthread_mutex_unlock(&camera->lock);
}

static int reply(uint8_t *data, size_t length, const uint8_t *src, size_t count) {
    if (count > length) count = length;
    if (count) memcpy(data, src, count);
    return (int)count;
}

int hv_uvc_control(HvUvc *camera, const uint8_t setup[8], uint8_t *data, size_t length) {
    const uint8_t type = setup[0], request = setup[1], index = setup[4];
    uint8_t value = setup[2], selector = setup[3];
    uint8_t small[64] = {0};
    if (type == 0x80 && request == 6) {
        switch (selector) {
        case 1: return reply(data, length, device, sizeof device);
        case 2:
        case 7: {
            uint8_t descriptor[sizeof config];
            memcpy(descriptor, config, sizeof config);
            descriptor[1] = selector;
            if (selector == 7) { /* other-speed bulk endpoint max packet */
                descriptor[sizeof config - 3] = 64;
                descriptor[sizeof config - 2] = 0;
            }
            return reply(data, length, descriptor, sizeof descriptor);
        }
        case 3:
            if (!value) return reply(data, length, (const uint8_t[]){4, 3, 9, 4}, 4);
            if (value > 2) return -1;
            const char *name = value == 1 ? "Henge" : "iPad Camera";
            small[0] = (uint8_t)(2 + 2 * strlen(name));
            small[1] = 3;
            for (size_t i = 0; name[i]; i++) small[2 + 2 * i] = (uint8_t)name[i];
            return reply(data, length, small, small[0]);
        case 6: return reply(data, length, qualifier, sizeof qualifier);
        default: return -1;
        }
    }
    if ((type == 0x80 || type == 0x81 || type == 0x82) && !request)
        return reply(data, length, small, 2);
    if (type == 0x80 && request == 8) return reply(data, length, &camera->config, 1);
    if (type == 0 && request == 9 && value <= 1) {
        hv_uvc_reset(camera);
        camera->config = value;
        return 0;
    }
    if (type == 0x81 && request == 10 && index <= 1) return reply(data, length, small, 1);
    if (type == 0x01 && request == 11 && index <= 1 && !value) return 0;
    if (type == 0x02 && request == 1 && !value) return 0; /* clear bulk endpoint halt */
    if (type == 0xa1 && index == 0 && !setup[5] && selector == 2 && request == 0x81)
        return reply(data, length, small, 1); /* request error code */
    if (index != 1 || setup[5] || (selector != 1 && selector != 2)) return -1;
    if (type == 0xa1) {
        switch (request) {
        case 0x81: case 0x82: case 0x83: case 0x84: case 0x87:
            return reply(data, length, probe, sizeof probe);
        case 0x85: small[0] = PROBE_BYTES; return reply(data, length, small, 2);
        case 0x86: small[0] = 3; return reply(data, length, small, 1);
        }
    }
    if (type != 0x21 || request != 1 || length < PROBE_BYTES || !data) return -1;
    /* Only one mode is offered. COMMIT must select it. */
    if (selector == 2) {
        if (data[2] != 1 || data[3] != 1) return -1;
        pthread_mutex_lock(&camera->lock);
        camera->committed = true;
        camera->offset = 0;
        camera->payload_left = 0;
        pthread_mutex_unlock(&camera->lock);
        fprintf(stderr, "henge: guest selected iPad camera mode\n");
    }
    return 0;
}

void hv_uvc_share(HvUvc *camera, bool enabled) {
    pthread_mutex_lock(&camera->lock);
    atomic_store_explicit(&camera->shared, enabled, memory_order_relaxed);
    if (!enabled) {
        fprintf(stderr, "henge: camera stopped (%llu frames captured, %llu delivered)\n",
                (unsigned long long)camera->submitted, (unsigned long long)camera->delivered);
        camera->submitted = camera->delivered = 0;
        camera->pending = false;
        camera->offset = 0;
        camera->payload_left = 0;
        memset(camera->frames, 0, sizeof camera->frames);
    }
    pthread_mutex_unlock(&camera->lock);
}

bool hv_uvc_shared(HvUvc *camera) {
    return atomic_load_explicit(&camera->shared, memory_order_relaxed);
}

bool hv_uvc_submit(HvUvc *camera, const uint8_t *y, size_t y_stride, const uint8_t *uv, size_t uv_stride) {
    if (!y || !uv || y_stride < HV_UVC_WIDTH || uv_stride < HV_UVC_WIDTH) return false;
    pthread_mutex_lock(&camera->lock);
    bool shared = hv_uvc_shared(camera);
    if (shared) {
        /* UVC NV12 is tightly packed: luma rows, followed by interleaved
           chroma rows. Preserve the native format and leave out host padding. */
        for (unsigned row = 0; row < HV_UVC_HEIGHT; row++)
            memcpy(camera->latest + row * HV_UVC_WIDTH, y + row * y_stride, HV_UVC_WIDTH);
        for (unsigned row = 0; row < HV_UVC_HEIGHT / 2; row++)
            memcpy(camera->latest + LUMA_BYTES + row * HV_UVC_WIDTH, uv + row * uv_stride, HV_UVC_WIDTH);
        camera->pending = true;
        if (++camera->submitted == 1) fprintf(stderr, "henge: first iPad camera frame captured\n");
    }
    pthread_mutex_unlock(&camera->lock);
    return shared;
}

size_t hv_uvc_payload(HvUvc *camera, uint8_t *data, size_t capacity) {
    if (capacity <= 2) return 0;
    pthread_mutex_lock(&camera->lock);
    if (!hv_uvc_shared(camera) || !camera->committed || (!camera->offset && !camera->pending)) {
        pthread_mutex_unlock(&camera->lock);
        return 0;
    }
    if (!camera->offset) {
        uint8_t *previous = camera->current;
        camera->current = camera->latest;
        camera->latest = previous;
        camera->pending = false;
        camera->fid ^= 1;
    }
    /* A USB read can end in the middle of a UVC payload. Continue its pixels
       on the next read without inserting a second header into the image. */
    size_t header = 0;
    if (!camera->payload_left) {
        size_t remaining = FRAME_BYTES - camera->offset;
        camera->payload_left = remaining < HV_UVC_PAYLOAD - 2 ? remaining : HV_UVC_PAYLOAD - 2;
        data[0] = 2;
        data[1] = 0x80 | camera->fid | (camera->payload_left == remaining ? 2 : 0);
        header = 2;
    }
    size_t bytes = camera->payload_left;
    if (bytes > capacity - header) bytes = capacity - header;
    memcpy(data + header, camera->current + camera->offset, bytes);
    camera->offset += bytes;
    camera->payload_left -= bytes;
    if (camera->offset == FRAME_BYTES) {
        camera->offset = 0;
        if (++camera->delivered == 1) fprintf(stderr, "henge: first camera frame delivered to macOS\n");
    }
    pthread_mutex_unlock(&camera->lock);
    return bytes + header;
}
