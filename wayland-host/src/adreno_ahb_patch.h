#ifndef TRIERARCH_ADRENO_AHB_PATCH_H
#define TRIERARCH_ADRENO_AHB_PATCH_H

#include "adreno_ahb_layout.h"
#include "adreno_ahb_native.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * Copies a short-lived Android donor handle and changes only the allocation
 * size fields whose offsets were discovered by runtime calibration.  The
 * donor's metadata FD is shared with the returned handle; the donor must not
 * be used anywhere else after this succeeds.
 */
bool trierarch_adreno_ahb_patch_allocation_size(
        const struct trierarch_adreno_ahb_layout *layout,
        const struct trierarch_adreno_native_handle *donor,
        uint64_t guest_bytes, struct trierarch_adreno_native_handle **patched);

#endif
