#ifndef TRIERARCH_ADRENO_AHB_FORGE_H
#define TRIERARCH_ADRENO_AHB_FORGE_H

#include "adreno_ahb_layout.h"
#include "adreno_ahb_native.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * Prepare a private, one-shot donor handle for the strict gpu probe.  The
 * donor must not be shared with a visible surface: its metadata fd is patched
 * in place before being sent through the GraphicBuffer wire format.
 */
bool trierarch_adreno_ahb_forge_prepare(
        const struct trierarch_adreno_ahb_layout *layout,
        const struct trierarch_adreno_native_handle *donor,
        uint32_t width, uint32_t height, uint32_t stride_bytes,
        uint64_t allocation_bytes,
        struct trierarch_adreno_native_handle **prepared);

void trierarch_adreno_ahb_forge_destroy(
        struct trierarch_adreno_native_handle *prepared);

#endif
