#ifndef TRIERARCH_ADRENO_AHB_LAYOUT_H
#define TRIERARCH_ADRENO_AHB_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Read-only description of the vendor allocator metadata discovered by
 * comparing two Android-owned AHardwareBuffer donors.  It deliberately does
 * not expose offsets: those remain implementation details until a later,
 * separately reviewed forge step needs them.
 */
struct trierarch_adreno_ahb_layout {
    bool candidate;
    uint32_t donor_format;
    int native_handle_fds;
    int native_handle_ints;
    uint64_t metadata_bytes;
    uint32_t first_stride;
    uint32_t second_stride;
    unsigned blob_width_matches;
    unsigned blob_height_matches;
    unsigned blob_stride_pixels_matches;
    unsigned blob_stride_bytes_matches;
    unsigned blob_size_matches;
    unsigned handle_width_matches;
    unsigned handle_height_matches;
    unsigned handle_stride_pixels_matches;
    unsigned handle_stride_bytes_matches;
    unsigned handle_size_matches;
};

/*
 * Allocates two Android-owned donors and reads their allocator metadata.  No
 * guest file descriptor is accepted, and no donor metadata is modified.
 */
bool trierarch_adreno_ahb_layout_calibrate(uint32_t ahb_format,
        struct trierarch_adreno_ahb_layout *result);

#endif
