#ifndef HENGEVM_UVC_H
#define HENGEVM_UVC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    HV_UVC_WIDTH = 640,
    HV_UVC_HEIGHT = 480,
    HV_UVC_FPS = 30,
    HV_UVC_PAYLOAD = 16384,
};

typedef struct HvUvc HvUvc;

HvUvc *hv_uvc_create(void);
void hv_uvc_destroy(HvUvc *camera);
void hv_uvc_reset(HvUvc *camera);
int hv_uvc_control(HvUvc *camera, const uint8_t setup[8], uint8_t *data, size_t length);
/* Capture thread. Input is video-range NV12; only the most recent frame is kept. */
bool hv_uvc_submit(HvUvc *camera, const uint8_t *y, size_t y_stride, const uint8_t *uv, size_t uv_stride);
/* UI thread. Turning off sharing clears the buffers before it returns. */
void hv_uvc_share(HvUvc *camera, bool enabled);
bool hv_uvc_shared(HvUvc *camera);
/* VM thread. Zero means the guest must wait for a frame. */
size_t hv_uvc_payload(HvUvc *camera, uint8_t *data, size_t capacity);

#endif
