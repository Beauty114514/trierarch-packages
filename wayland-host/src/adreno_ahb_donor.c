#include "adreno_ahb_donor.h"

#include "adreno_ahb_native.h"

#include <android/hardware_buffer.h>
#include <android/log.h>

#include <sys/stat.h>
#include <string.h>

#define TAG "TrierarchAdrenoAhb"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

static uint64_t fd_bytes(int fd) {
    struct stat status = {0};
    return fd >= 0 && fstat(fd, &status) == 0 && status.st_size > 0
            ? (uint64_t)status.st_size : 0;
}

bool trierarch_adreno_ahb_compare_donor(int guest_fd, uint32_t width,
        uint32_t height, uint32_t stride_bytes, uint32_t ahb_format,
        struct trierarch_adreno_ahb_donor_match *result) {
    if (!result || guest_fd < 0 || !width || !height || !stride_bytes)
        return false;
    memset(result, 0, sizeof(*result));
    result->guest_bytes = fd_bytes(guest_fd);

    const AHardwareBuffer_Desc request = {
        .width = width,
        .height = height,
        .layers = 1,
        .format = ahb_format,
        .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
    };
    AHardwareBuffer *donor = NULL;
    if (AHardwareBuffer_allocate(&request, &donor) != 0 || !donor)
        return false;

    AHardwareBuffer_Desc description = {0};
    AHardwareBuffer_describe(donor, &description);
    const struct trierarch_adreno_native_handle *handle =
            trierarch_adreno_ahb_native_handle(donor);
    if (handle) {
        result->native_handle_fds = handle->num_fds;
        result->native_handle_ints = handle->num_ints;
        if (handle->num_fds >= 1)
            result->donor_pixel_bytes = fd_bytes(handle->data[0]);
        if (handle->num_fds >= 2)
            result->donor_metadata_bytes = fd_bytes(handle->data[1]);
    }
    result->donor_stride_bytes = description.stride <= UINT32_MAX / 4
            ? description.stride * 4 : 0;
    result->exact = description.width == width && description.height == height &&
            description.format == ahb_format && result->donor_stride_bytes == stride_bytes &&
            result->native_handle_fds == 2 && result->guest_bytes &&
            result->guest_bytes == result->donor_pixel_bytes &&
            result->donor_metadata_bytes;
    LOGI("implicit donor comparison: guest=%ux%u stride=%u bytes=%llu donor="
            "%ux%u stride=%u pixel-bytes=%llu metadata-bytes=%llu fds=%d ints=%d exact=%d",
            width, height, stride_bytes, (unsigned long long)result->guest_bytes,
            description.width, description.height, result->donor_stride_bytes,
            (unsigned long long)result->donor_pixel_bytes,
            (unsigned long long)result->donor_metadata_bytes,
            result->native_handle_fds, result->native_handle_ints, result->exact);
    AHardwareBuffer_release(donor);
    return true;
}
