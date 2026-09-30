#ifndef TRIERARCH_ADRENO_AHB_MATRIX_H
#define TRIERARCH_ADRENO_AHB_MATRIX_H

#include <stdint.h>

/* Read-only allocator comparison for one guest GPU probe buffer. */
void trierarch_adreno_ahb_matrix_run(int guest_fd, uint32_t width,
        uint32_t height, uint32_t stride_bytes, uint32_t drm_format,
        uint64_t modifier);

#endif
