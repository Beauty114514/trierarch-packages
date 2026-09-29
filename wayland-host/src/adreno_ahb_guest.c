#include "adreno_ahb_guest.h"

#include "adreno_ahb_native.h"
#include "adreno_ahb_patch.h"
#include "adreno_ahb_wire.h"

#include <android/hardware_buffer.h>
#include <android/log.h>

#include <stdlib.h>
#include <sys/stat.h>

#define TAG "TrierarchAdrenoAhb"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

static bool describe_matches(const AHardwareBuffer_Desc *expected,
        const AHardwareBuffer_Desc *actual) {
    return expected->width == actual->width && expected->height == actual->height &&
            expected->layers == actual->layers && expected->format == actual->format &&
            expected->stride == actual->stride && expected->usage == actual->usage;
}

static uint64_t fd_size(int fd) {
    struct stat status = {0};
    return fd >= 0 && fstat(fd, &status) == 0 && status.st_size > 0
            ? (uint64_t)status.st_size : 0;
}

bool trierarch_adreno_ahb_guest_fd_import(
        const struct trierarch_adreno_ahb_layout *layout, int guest_fd,
        uint32_t width, uint32_t height, uint32_t stride_bytes,
        uint32_t ahb_format, AHardwareBuffer **buffer) {
    if (!layout || layout->donor_format != ahb_format || !buffer || guest_fd < 0 ||
            !width || !height || !stride_bytes)
        return false;
    *buffer = NULL;
    const AHardwareBuffer_Desc requested = {
        .width = width,
        .height = height,
        .layers = 1,
        .format = ahb_format,
        .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
    };
    AHardwareBuffer *donor = NULL;
    AHardwareBuffer *received = NULL;
    struct trierarch_adreno_native_handle *patched_handle = NULL;
    bool success = false;
    if (AHardwareBuffer_allocate(&requested, &donor) != 0 || !donor)
        goto out;

    AHardwareBuffer_Desc description = {0};
    AHardwareBuffer_describe(donor, &description);
    const struct trierarch_adreno_native_handle *handle =
            trierarch_adreno_ahb_native_handle(donor);
    if (!handle || handle->num_fds != 2) {
        LOGW("AHB guest probe rejected donor handle");
        goto out;
    }
    uint64_t guest_bytes = fd_size(guest_fd);
    uint64_t donor_bytes = fd_size(handle->data[0]);
    bool geometry_matches = description.width == width && description.height == height &&
            description.stride <= UINT32_MAX / 4 && description.stride * 4 == stride_bytes &&
            guest_bytes;
    LOGI("AHB geometry probe: guest=%ux%u stride=%u bytes=%llu donor=%ux%u stride=%u "
            "bytes=%llu match=%d", width, height, stride_bytes,
            (unsigned long long)guest_bytes, description.width, description.height,
            description.stride * 4, (unsigned long long)donor_bytes,
            geometry_matches && guest_bytes == donor_bytes);
    if (!geometry_matches)
        goto out;

    const struct trierarch_adreno_native_handle *registered_handle = handle;
    if (guest_bytes != donor_bytes) {
        if (!trierarch_adreno_ahb_patch_allocation_size(layout, handle, guest_bytes,
                    &patched_handle)) {
            LOGW("AHB guest probe refused uncalibrated allocation-size mismatch");
            goto out;
        }
        registered_handle = patched_handle;
    }
    if (!trierarch_adreno_ahb_register_handle(&description, registered_handle, guest_fd,
                handle->data[1], &received))
        goto out;

    AHardwareBuffer_Desc received_description = {0};
    AHardwareBuffer_describe(received, &received_description);
    success = describe_matches(&description, &received_description);
    LOGI("AHB guest pixel-FD import: success=%d %ux%u stride=%u format=%u",
            success, received_description.width, received_description.height,
            received_description.stride, received_description.format);
    if (success) {
        *buffer = received;
        received = NULL;
    }

out:
    free(patched_handle);
    if (received)
        AHardwareBuffer_release(received);
    if (donor)
        AHardwareBuffer_release(donor);
    if (!success)
        LOGW("AHB guest pixel-FD import failed");
    return success;
}
