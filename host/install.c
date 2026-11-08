/* Installing macOS on a fresh virtual Mac, the way Virtualization.framework's
   VZMacOSInstaller does: the machine boots its ROM in DFU mode and shows up
   as a USB device, and a standard restore (personalized with Apple's signing
   server for this machine's ECID, then the restore OS writes the disk) runs
   over that link. The vendored restore tools use host/libusb_shim.c to talk to
   the guest. The boot-ROM exploit is left out, and usbmuxd is limited to this
   process. Nothing of Apple's is
   decrypted or patched here: encrypted images go to the guest as they are, and
   the restore OS asks for its keys itself (the host just fetches the URL). */

#include "install.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <removefile.h>
#include <sys/un.h>

#include "idevicerestore/src/idevicerestore.h"
#include "usb_host.h"

int usbmuxd_main(int argc, char **argv);
void usbmuxd_prepare(void);
void usbmuxd_stop(void);

static HvInstallProgress g_progress;
static void *g_progress_user;
static char g_ipsw[1024], g_cache[1024];
static char g_mux_dir[sizeof(((struct sockaddr_un *)0)->sun_path)];
static char g_mux[sizeof g_mux_dir], g_records[sizeof g_mux_dir + 8];
static pthread_t g_mux_thread;
static uint64_t g_ecid;
static volatile int g_result = 1; /* 1 running, 0 done, < 0 failed */

static void *mux_thread(void *arg) {
    (void)arg;
    char *argv[] = {"usbmuxd", "-f", "-p", "-P", "NONE", "-S", g_mux, "-C", g_records, NULL};
    int argc = 9;
    if (getenv("HV_INSTALL_LOG")) {
        static char *v[] = {"usbmuxd", "-f", "-v", "-v", "-p", "-P", "NONE", "-S", NULL, "-C", NULL, NULL};
        v[8] = g_mux, v[10] = g_records;
        usbmuxd_main(11, v);
    } else {
        usbmuxd_main(argc, argv);
    }
    fprintf(stderr, "install: usbmuxd stopped\n");
    return NULL;
}

static void stop_mux(void) {
    usbmuxd_stop();
    pthread_join(g_mux_thread, NULL);
    removefile(g_mux_dir, NULL, REMOVEFILE_RECURSIVE);
}

static void progress(int step, double p, void *user) {
    (void)user;
    if (g_progress) g_progress(step, p, g_progress_user);
}

static void *restore_thread(void *arg) {
    (void)arg;
    struct idevicerestore_client_t *c = idevicerestore_client_new();
    if (!c) {
        stop_mux();
        g_result = -1;
        if (g_progress) g_progress(HV_INSTALL_FAILED, 1.0, g_progress_user);
        return NULL;
    }
    int flags = FLAG_ERASE;
    if (getenv("HV_INSTALL_LOG")) flags |= FLAG_DEBUG;
    idevicerestore_set_flags(c, flags);
    idevicerestore_set_ipsw(c, g_ipsw);
    idevicerestore_set_cache_path(c, g_cache);
    if (g_ecid) idevicerestore_set_ecid(c, g_ecid);
    idevicerestore_set_progress_callback(c, progress, NULL);
    int r = idevicerestore_start(c);
    fprintf(stderr, "install: restore finished: %d\n", r);
    idevicerestore_client_free(c);
    stop_mux();
    g_result = r == 0 ? 0 : -1;
    if (g_progress) g_progress(r == 0 ? HV_INSTALL_DONE : HV_INSTALL_FAILED, 1.0, g_progress_user);
    return NULL;
}

int hv_install_start(HvVirtioUsb *usb, const char *ipsw, const char *workdir, const char *tempdir,
                     uint64_t ecid, HvInstallProgress cb, void *user) {
    if (!usb || !tempdir || tempdir[0] != '/') return -1;
    snprintf(g_ipsw, sizeof g_ipsw, "%s", ipsw);
    snprintf(g_cache, sizeof g_cache, "%s", workdir);

    /* The socket and pairing records live in a fresh mode-0700 directory.
       The embedded daemon also checks the peer PID on every connection. */
    int dirlen = snprintf(g_mux_dir, sizeof g_mux_dir, "%s/h.XXXXXX", tempdir);
    if (dirlen < 0 || (size_t)dirlen + sizeof "/s" > sizeof g_mux || !mkdtemp(g_mux_dir)) return -1;
    snprintf(g_mux, sizeof g_mux, "%s/s", g_mux_dir);
    snprintf(g_records, sizeof g_records, "%s/records", g_mux_dir);
    char address[sizeof g_mux + sizeof "UNIX:"];
    snprintf(address, sizeof address, "UNIX:%s", g_mux);
    if (setenv("USBMUXD_SOCKET_ADDRESS", address, 1) != 0) {
        rmdir(g_mux_dir);
        return -1;
    }
    g_ecid = ecid;
    g_progress = cb;
    g_progress_user = user;
    hv_libusb_shim_attach(usb);
    pthread_t t;
    usbmuxd_prepare();
    if (pthread_create(&g_mux_thread, NULL, mux_thread, NULL) != 0) {
        rmdir(g_mux_dir);
        return -1;
    }
    usleep(300000);
    if (pthread_create(&t, NULL, restore_thread, NULL) != 0) {
        stop_mux();
        return -1;
    }
    pthread_detach(t);
    return 0;
}

int hv_install_result(void) { return g_result; }
