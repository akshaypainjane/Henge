#ifndef HENGEVM_PVG_DIAGNOSTICS_H
#define HENGEVM_PVG_DIAGNOSTICS_H

#include <TargetConditionals.h>
#include <stdlib.h>

#if !TARGET_OS_IPHONE || defined(HENGE_IOS_DIAGNOSTICS)
#define HENGE_PVG_DIAGNOSTICS 1
#else
#define HENGE_PVG_DIAGNOSTICS 0
#endif

static inline const char *hv_pvg_diagnostic_env(const char *name) {
#if HENGE_PVG_DIAGNOSTICS
    return getenv(name);
#else
    (void)name;
    return NULL;
#endif
}

/* A diagnostic can retain pixels after capture stops. Exclude live camera
   sharing for the entire debug launch when any frame capture is enabled. */
static inline int hv_pvg_frame_diagnostics_enabled(void) {
    return hv_pvg_diagnostic_env("HV_COLOR_PROBE") || hv_pvg_diagnostic_env("HV_PVG_SURFACE_DUMP");
}

#endif
