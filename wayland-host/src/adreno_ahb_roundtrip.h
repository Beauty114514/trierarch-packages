#ifndef TRIERARCH_ADRENO_AHB_ROUNDTRIP_H
#define TRIERARCH_ADRENO_AHB_ROUNDTRIP_H

#include <stdbool.h>
#include <stdint.h>

/* Test-only: re-register an unchanged Android-owned AHardwareBuffer. */
bool trierarch_adreno_ahb_roundtrip_run(uint32_t ahb_format);

#endif
