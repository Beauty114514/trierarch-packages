#ifndef TRIERARCH_ADRENO_AHB_PADDED_H
#define TRIERARCH_ADRENO_AHB_PADDED_H

#include <android/hardware_buffer.h>

#include <stdbool.h>
#include <stdint.h>

/* The donor keeps its original metadata and uses the allocation height
 * implied by the guest FD, not the visible image height. */
bool trierarch_adreno_ahb_padded_import(int guest_fd, uint32_t width,
        uint32_t visible_height, uint32_t stride_bytes, uint32_t ahb_format,
        AHardwareBuffer **buffer, uint32_t *allocation_height);

/* Accepts an exact visible donor too; used by the normal linear dma-buf path. */
bool trierarch_adreno_ahb_linear_import(int guest_fd, uint32_t width,
        uint32_t visible_height, uint32_t stride_bytes, uint32_t ahb_format,
        AHardwareBuffer **buffer, uint32_t *allocation_height);

#endif
