#ifndef TRIERARCH_ADRENO_AHB_LAYOUT_H
#define TRIERARCH_ADRENO_AHB_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Read-only description of the vendor allocator metadata discovered by
 * comparing three Android-owned AHardwareBuffer donors.  Offset candidates
 * are retained for diagnostics only; this module never writes them.
 */
#define TRIERARCH_ADRENO_AHB_MAX_OFFSETS 8

struct trierarch_adreno_ahb_offsets {
    uint32_t values[TRIERARCH_ADRENO_AHB_MAX_OFFSETS];
    unsigned count;
};

struct trierarch_adreno_ahb_layout {
    bool candidate;
    bool all_fields_observed; /* diagnostic only; does not authorize handle forgery */
    uint32_t donor_format;
    uint64_t donor_usage;
    int native_handle_fds;
    int native_handle_ints;
    uint64_t metadata_bytes;
    uint32_t first_stride;
    uint32_t second_stride;
    struct trierarch_adreno_ahb_offsets blob_width;
    struct trierarch_adreno_ahb_offsets blob_height;
    struct trierarch_adreno_ahb_offsets blob_stride_pixels;
    struct trierarch_adreno_ahb_offsets blob_stride_bytes;
    struct trierarch_adreno_ahb_offsets blob_size;
    struct trierarch_adreno_ahb_offsets blob_exact_size;
    struct trierarch_adreno_ahb_offsets blob_extent; /* allocation size + fixed delta */
    uint32_t blob_extent_delta;
    struct trierarch_adreno_ahb_offsets handle_width;
    struct trierarch_adreno_ahb_offsets handle_height;
    struct trierarch_adreno_ahb_offsets handle_stride_pixels;
    struct trierarch_adreno_ahb_offsets handle_stride_bytes;
    struct trierarch_adreno_ahb_offsets handle_size;
};

/*
 * Allocates three Android-owned donors and reads their allocator metadata. No
 * guest file descriptor is accepted, and no donor metadata is modified.
 */
bool trierarch_adreno_ahb_layout_calibrate(uint32_t ahb_format,
        struct trierarch_adreno_ahb_layout *result);
bool trierarch_adreno_ahb_layout_calibrate_for_usage(uint32_t ahb_format,
        uint64_t usage, struct trierarch_adreno_ahb_layout *result);

#endif
