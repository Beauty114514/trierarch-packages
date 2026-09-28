#ifndef TRIERARCH_ADRENO_AHB_NATIVE_H
#define TRIERARCH_ADRENO_AHB_NATIVE_H

#include <android/hardware_buffer.h>
#include <stdbool.h>

/* VNDK exposes this through libnativewindow, but it is not an NDK API. */
struct trierarch_adreno_native_handle {
    int version;
    int num_fds;
    int num_ints;
    int data[];
};

const struct trierarch_adreno_native_handle *
trierarch_adreno_ahb_native_handle(const AHardwareBuffer *buffer);

bool trierarch_adreno_ahb_native_handle_available(void);

#endif
