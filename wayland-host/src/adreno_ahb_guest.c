#include "adreno_ahb_guest.h"

#include "adreno_ahb_native.h"
#include "adreno_ahb_wire.h"

#include <android/hardware_buffer.h>
#include <android/log.h>

#define TAG "TrierarchAdrenoAhb"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

static bool describe_matches(const AHardwareBuffer_Desc *expected,
        const AHardwareBuffer_Desc *actual) {
    return expected->width == actual->width && expected->height == actual->height &&
            expected->layers == actual->layers && expected->format == actual->format &&
            expected->stride == actual->stride && expected->usage == actual->usage;
}

bool trierarch_adreno_ahb_guest_fd_probe(int guest_fd, uint32_t width, uint32_t height,
        uint32_t stride_bytes, uint32_t ahb_format) {
    if (guest_fd < 0 || !width || !height || !stride_bytes)
        return false;
    const AHardwareBuffer_Desc requested = {
        .width = width,
        .height = height,
        .layers = 1,
        .format = ahb_format,
        .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
    };
    AHardwareBuffer *donor = NULL;
    AHardwareBuffer *received = NULL;
    bool success = false;
    if (AHardwareBuffer_allocate(&requested, &donor) != 0 || !donor)
        goto out;

    AHardwareBuffer_Desc description = {0};
    AHardwareBuffer_describe(donor, &description);
    if (description.width != width || description.height != height ||
            description.stride > UINT32_MAX / 4 || description.stride * 4 != stride_bytes) {
        LOGW("AHB guest probe requires matching donor geometry: guest=%ux%u stride=%u "
                "donor=%ux%u stride=%u", width, height, stride_bytes,
                description.width, description.height, description.stride * 4);
        goto out;
    }
    const struct trierarch_adreno_native_handle *handle =
            trierarch_adreno_ahb_native_handle(donor);
    if (!handle || handle->num_fds != 2) {
        LOGW("AHB guest probe rejected donor handle");
        goto out;
    }
    if (!trierarch_adreno_ahb_register_handle(&description, handle, guest_fd,
                handle->data[1], &received))
        goto out;

    AHardwareBuffer_Desc received_description = {0};
    AHardwareBuffer_describe(received, &received_description);
    success = describe_matches(&description, &received_description);
    LOGI("AHB guest pixel-FD probe: success=%d %ux%u stride=%u format=%u",
            success, received_description.width, received_description.height,
            received_description.stride, received_description.format);

out:
    if (received)
        AHardwareBuffer_release(received);
    if (donor)
        AHardwareBuffer_release(donor);
    if (!success)
        LOGW("AHB guest pixel-FD probe failed");
    return success;
}
