#ifndef TRIERARCH_ADRENO_AHB_IMPORT_H
#define TRIERARCH_ADRENO_AHB_IMPORT_H

#include "adreno_ahb_layout.h"

/*
 * This is deliberately a preflight rather than an importer. It identifies
 * the Android allocator layout needed by the experimental QCOM DMA-BUF → AHB
 * adapter without receiving, modifying, or registering a guest buffer.
 */
struct trierarch_adreno_ahb_preflight {
    bool candidate;
    bool donor_roundtrip;
    uint32_t donor_format;
    uint32_t first_stride;
    uint32_t second_stride;
    int native_handle_fds;
    int native_handle_ints;
    uint64_t metadata_bytes;
    struct trierarch_adreno_ahb_layout layout;
};

bool trierarch_adreno_ahb_preflight_run(uint32_t width, uint32_t height,
        uint32_t drm_format, struct trierarch_adreno_ahb_preflight *result);

#endif
