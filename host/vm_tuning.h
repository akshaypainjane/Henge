#ifndef HENGEVM_VM_TUNING_H
#define HENGEVM_VM_TUNING_H

#include <stdbool.h>
#include <stdint.h>

/* The app supplies one snapshot when creating a VM. CLI tools can continue to
   use their existing environment and command-line settings. */
typedef struct HvVmTuning {
    uint32_t refresh_hz;
    int stats_interval;
    bool fast_disk_flush;
    bool uncached_disk_writes;
    bool idle_spin;
    bool superblocks;
} HvVmTuning;

#endif
