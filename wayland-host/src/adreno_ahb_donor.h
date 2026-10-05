#ifndef TRIERARCH_ADRENO_AHB_DONOR_H
#define TRIERARCH_ADRENO_AHB_DONOR_H

#include <stdbool.h>
#include <stdint.h>

struct trierarch_adreno_ahb_donor_match {
    bool exact;
    int native_handle_fds;
    int native_handle_ints;
    uint32_t donor_stride_bytes;
    uint64_t guest_bytes;
    uint64_t donor_pixel_bytes;
    uint64_t donor_metadata_bytes;
};

/* Allocates and immediately releases an Android-owned donor. It never
 * registers, modifies, or imports the guest FD. */
bool trierarch_adreno_ahb_compare_donor(int guest_fd, uint32_t width,
        uint32_t height, uint32_t stride_bytes, uint32_t ahb_format,
        struct trierarch_adreno_ahb_donor_match *result);

#endif
