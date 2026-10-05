#ifndef TRIERARCH_ADRENO_AHB_GUEST_H
#define TRIERARCH_ADRENO_AHB_GUEST_H

#include "adreno_ahb_layout.h"

#include <stdbool.h>
#include <stdint.h>
#include <android/hardware_buffer.h>

/* Test-only: replace an unchanged donor pixel FD with a guest DMA-BUF whose
 * capacity is at least the donor's logical allocation. */
bool trierarch_adreno_ahb_guest_fd_import(
        const struct trierarch_adreno_ahb_layout *layout, int guest_fd,
        uint32_t width, uint32_t height, uint32_t stride_bytes,
        uint32_t ahb_format, AHardwareBuffer **buffer);

#endif
