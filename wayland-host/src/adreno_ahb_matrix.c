#include "adreno_ahb_matrix.h"

#include "adreno_ahb_layout.h"
#include "adreno_ahb_native.h"

#include <android/hardware_buffer.h>
#include <android/log.h>

#include <stdbool.h>
#include <sys/stat.h>

#define TAG "TrierarchAdrenoAhb"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

/* Android platform graphics.h defines BGRA_8888 as 5; the NDK omits it. */
#define TRIERARCH_AHB_FORMAT_BGRA_8888 5u

struct matrix_variant {
    const char *name;
    uint32_t format;
    uint64_t usage;
};

static uint64_t fd_bytes(int fd) {
    struct stat status = {0};
    return fd >= 0 && fstat(fd, &status) == 0 && status.st_size > 0
            ? (uint64_t)status.st_size : 0;
}

static void measure_variant(const struct matrix_variant *variant,
        uint32_t width, uint32_t height, uint32_t guest_stride,
        uint64_t guest_bytes) {
    struct trierarch_adreno_ahb_layout layout = {0};
    bool calibrated = trierarch_adreno_ahb_layout_calibrate_for_usage(
            variant->format, variant->usage, &layout);
    AHardwareBuffer_Desc request = {
        .width = width,
        .height = height,
        .layers = 1,
        .format = variant->format,
        .usage = variant->usage,
    };
    AHardwareBuffer *donor = NULL;
    int allocation = AHardwareBuffer_allocate(&request, &donor);
    if (allocation || !donor) {
        LOGI("AHB matrix %s: allocation failed rc=%d calibrated=%d",
                variant->name, allocation, calibrated);
        if (donor)
            AHardwareBuffer_release(donor);
        return;
    }

    AHardwareBuffer_Desc description = {0};
    AHardwareBuffer_describe(donor, &description);
    const struct trierarch_adreno_native_handle *handle =
            trierarch_adreno_ahb_native_handle(donor);
    int fds = handle ? handle->num_fds : -1;
    int ints = handle ? handle->num_ints : -1;
    uint64_t pixels = fds >= 1 ? fd_bytes(handle->data[0]) : 0;
    uint64_t metadata = fds >= 2 ? fd_bytes(handle->data[1]) : 0;
    uint64_t logical = (uint64_t)description.stride * 4 * height;
    uint64_t donor_stride = (uint64_t)description.stride * 4;
    LOGI("AHB matrix %s: alloc=%ux%u fmt=%u usage=0x%llx stride=%llu logical=%llu fd=%llu "
            "metadata=%llu fds=%d ints=%d guest_stride_match=%d guest_fd_match=%d "
            "calibrated=%d fields=%d all_fields=%d exact=%u extent=%u+%u handle_sb=%u",
            variant->name, width, height, variant->format,
            (unsigned long long)variant->usage,
            (unsigned long long)donor_stride, (unsigned long long)logical,
            (unsigned long long)pixels, (unsigned long long)metadata, fds, ints,
            donor_stride == guest_stride, pixels && pixels == guest_bytes,
            calibrated, layout.candidate, layout.all_fields_observed,
            layout.blob_exact_size.count, layout.blob_extent.count,
            layout.blob_extent_delta, layout.handle_stride_bytes.count);
    AHardwareBuffer_release(donor);
}

void trierarch_adreno_ahb_matrix_run(int guest_fd, uint32_t width,
        uint32_t height, uint32_t stride_bytes, uint32_t drm_format,
        uint64_t modifier) {
    if (guest_fd < 0 || !width || !height || !stride_bytes)
        return;
    uint64_t guest_bytes = fd_bytes(guest_fd);
    uint64_t logical = (uint64_t)stride_bytes * height;
    LOGI("AHB matrix guest: %ux%u drm=0x%x modifier=0x%llx stride=%u "
            "logical=%llu fd=%llu spare=%lld",
            width, height, drm_format, (unsigned long long)modifier, stride_bytes,
            (unsigned long long)logical, (unsigned long long)guest_bytes,
            (long long)guest_bytes - (long long)logical);

    const uint64_t sampled = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    const uint64_t framebuffer = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER;
    const struct matrix_variant variants[] = {
        { "rgba-sampled", AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, sampled },
        { "rgba-renderable", AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
            sampled | framebuffer },
        { "bgra-sampled", TRIERARCH_AHB_FORMAT_BGRA_8888, sampled },
        { "bgra-renderable", TRIERARCH_AHB_FORMAT_BGRA_8888,
            sampled | framebuffer },
    };
    for (unsigned index = 0; index < sizeof(variants) / sizeof(variants[0]); ++index)
        measure_variant(&variants[index], width, height, stride_bytes, guest_bytes);

    /* Test the capacity implied by the guest FD without changing the visible
     * image geometry or attempting to import the padded donor. */
    if (guest_bytes > logical && guest_bytes % stride_bytes == 0) {
        uint64_t inferred_height = guest_bytes / stride_bytes;
        if (inferred_height > height && inferred_height <= 4096) {
            const struct matrix_variant padded = {
                "rgba-padded", AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, sampled,
            };
            LOGI("AHB matrix padded: visible=%ux%u inferred_alloc_height=%llu",
                    width, height, (unsigned long long)inferred_height);
            measure_variant(&padded, width, (uint32_t)inferred_height,
                    stride_bytes, guest_bytes);
        }
    }
}
