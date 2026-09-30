#include "adreno_ahb_padded.h"

#include "adreno_ahb_native.h"
#include "adreno_ahb_wire.h"

#include <android/log.h>

#include <sys/stat.h>

#define TAG "TrierarchAdrenoAhb"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

static uint64_t fd_bytes(int fd) {
    struct stat status = {0};
    return fd >= 0 && fstat(fd, &status) == 0 && status.st_size > 0
            ? (uint64_t)status.st_size : 0;
}

static bool description_matches(const AHardwareBuffer_Desc *expected,
        const AHardwareBuffer_Desc *actual) {
    return expected->width == actual->width && expected->height == actual->height &&
            expected->layers == actual->layers && expected->format == actual->format &&
            expected->stride == actual->stride && expected->usage == actual->usage;
}

bool trierarch_adreno_ahb_padded_import(int guest_fd, uint32_t width,
        uint32_t visible_height, uint32_t stride_bytes, uint32_t ahb_format,
        AHardwareBuffer **buffer, uint32_t *allocation_height) {
    if (!buffer || !allocation_height || guest_fd < 0 || !width ||
            !visible_height || !stride_bytes)
        return false;
    *buffer = NULL;
    *allocation_height = 0;

    uint64_t guest_bytes = fd_bytes(guest_fd);
    uint64_t padded_height = guest_bytes / stride_bytes;
    if (guest_bytes <= (uint64_t)stride_bytes * visible_height ||
            guest_bytes % stride_bytes || padded_height > 4096 ||
            padded_height - visible_height > 64)
        return false;

    const AHardwareBuffer_Desc request = {
        .width = width,
        .height = (uint32_t)padded_height,
        .layers = 1,
        .format = ahb_format,
        .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
    };
    AHardwareBuffer_Desc visible_request = request;
    visible_request.height = visible_height;
    AHardwareBuffer *visible_donor = NULL;
    int visible_status = AHardwareBuffer_allocate(&visible_request, &visible_donor);
    if (visible_status == 0 && visible_donor) {
        AHardwareBuffer_Desc visible = {0};
        AHardwareBuffer_describe(visible_donor, &visible);
        const struct trierarch_adreno_native_handle *visible_handle =
                trierarch_adreno_ahb_native_handle(visible_donor);
        bool already_matches = visible.stride <= UINT32_MAX / 4 &&
                visible.stride * 4 == stride_bytes && visible_handle &&
                visible_handle->num_fds >= 1 &&
                fd_bytes(visible_handle->data[0]) == guest_bytes;
        AHardwareBuffer_release(visible_donor);
        if (already_matches)
            return false;
    } else if (visible_donor)
        AHardwareBuffer_release(visible_donor);

    AHardwareBuffer *donor = NULL;
    AHardwareBuffer *received = NULL;
    bool success = false;
    int status = AHardwareBuffer_allocate(&request, &donor);
    if (status || !donor) {
        LOGW("AHB padded probe donor allocation failed: rc=%d", status);
        goto out;
    }

    AHardwareBuffer_Desc actual = {0};
    AHardwareBuffer_describe(donor, &actual);
    const struct trierarch_adreno_native_handle *handle =
            trierarch_adreno_ahb_native_handle(donor);
    uint64_t donor_bytes = handle && handle->num_fds >= 1
            ? fd_bytes(handle->data[0]) : 0;
    bool geometry_matches = actual.width == width &&
            actual.height == padded_height && actual.format == ahb_format &&
            actual.stride <= UINT32_MAX / 4 &&
            actual.stride * 4 == stride_bytes && donor_bytes == guest_bytes &&
            handle && handle->num_fds == 2 && handle->data[1] >= 0;
    LOGI("AHB padded probe geometry: visible=%ux%u alloc=%ux%u "
            "stride=%u guest_fd=%llu donor_fd=%llu match=%d",
            width, visible_height, actual.width, actual.height, stride_bytes,
            (unsigned long long)guest_bytes, (unsigned long long)donor_bytes,
            geometry_matches);
    if (!geometry_matches)
        goto out;

    LOGI("AHB padded probe registering unmodified donor metadata");
    if (!trierarch_adreno_ahb_register_handle(&actual, handle, guest_fd,
                handle->data[1], &received)) {
        LOGW("AHB padded probe registration failed");
        goto out;
    }
    AHardwareBuffer_Desc imported = {0};
    AHardwareBuffer_describe(received, &imported);
    if (!description_matches(&actual, &imported)) {
        LOGW("AHB padded probe registered description mismatch");
        goto out;
    }

    LOGI("AHB padded probe registration succeeded: alloc=%ux%u visible=%ux%u",
            imported.width, imported.height, width, visible_height);
    *buffer = received;
    *allocation_height = imported.height;
    received = NULL;
    success = true;

out:
    if (received)
        AHardwareBuffer_release(received);
    if (donor)
        AHardwareBuffer_release(donor);
    return success;
}
