#ifndef TRIERARCH_ADRENO_AHB_ROUNDTRIP_H
#define TRIERARCH_ADRENO_AHB_ROUNDTRIP_H

#include <stdbool.h>
#include <stdint.h>

/* Test-only: re-register an unchanged Android-owned AHardwareBuffer. */
bool trierarch_adreno_ahb_roundtrip_run(uint32_t ahb_format);

/* Test-only: use pixel storage from one Android donor with a separate
 * same-geometry donor's metadata and handle. */
bool trierarch_adreno_ahb_cross_donor_run(uint32_t ahb_format,
        uint32_t width, uint32_t height);

#endif
