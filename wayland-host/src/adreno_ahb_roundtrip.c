#include "adreno_ahb_roundtrip.h"

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

bool trierarch_adreno_ahb_roundtrip_run(uint32_t ahb_format) {
    const AHardwareBuffer_Desc requested = {
        .width = 64,
        .height = 64,
        .layers = 1,
        .format = ahb_format,
        .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
    };
    AHardwareBuffer *source = NULL;
    AHardwareBuffer *received = NULL;
    bool success = false;

    if (AHardwareBuffer_allocate(&requested, &source) != 0 || !source)
        goto out;
    const struct trierarch_adreno_native_handle *handle =
            trierarch_adreno_ahb_native_handle(source);
    if (!handle || handle->num_fds != 2) {
        LOGW("AHB round-trip rejected donor layout fds=%d ints=%d",
                handle ? handle->num_fds : -1, handle ? handle->num_ints : -1);
        goto out;
    }

    AHardwareBuffer_Desc description = {0};
    AHardwareBuffer_describe(source, &description);
    if (!trierarch_adreno_ahb_register_handle(&description, handle,
                handle->data[0], handle->data[1], &received))
        goto out;

    AHardwareBuffer_Desc received_description = {0};
    AHardwareBuffer_describe(received, &received_description);
    success = describe_matches(&description, &received_description);
    LOGI("AHB donor round-trip: success=%d %ux%u stride=%u format=%u",
            success, received_description.width, received_description.height,
            received_description.stride, received_description.format);

out:
    if (received)
        AHardwareBuffer_release(received);
    if (source)
        AHardwareBuffer_release(source);
    if (!success)
        LOGW("AHB donor round-trip failed");
    return success;
}

bool trierarch_adreno_ahb_cross_donor_run(uint32_t ahb_format,
        uint32_t width, uint32_t height) {
    const AHardwareBuffer_Desc requested = {
        .width = width,
        .height = height,
        .layers = 1,
        .format = ahb_format,
        .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
    };
    AHardwareBuffer *metadata_donor = NULL;
    AHardwareBuffer *pixel_donor = NULL;
    AHardwareBuffer *received = NULL;
    bool success = false;

    if (!width || !height || AHardwareBuffer_allocate(&requested, &metadata_donor) != 0 ||
            !metadata_donor || AHardwareBuffer_allocate(&requested, &pixel_donor) != 0 ||
            !pixel_donor)
        goto out;
    const struct trierarch_adreno_native_handle *metadata_handle =
            trierarch_adreno_ahb_native_handle(metadata_donor);
    const struct trierarch_adreno_native_handle *pixel_handle =
            trierarch_adreno_ahb_native_handle(pixel_donor);
    if (!metadata_handle || !pixel_handle || metadata_handle->num_fds != 2 ||
            pixel_handle->num_fds != 2 ||
            metadata_handle->num_ints != pixel_handle->num_ints) {
        LOGW("AHB cross-donor rejected donor layouts");
        goto out;
    }

    AHardwareBuffer_Desc description = {0};
    AHardwareBuffer_Desc pixel_description = {0};
    AHardwareBuffer_describe(metadata_donor, &description);
    AHardwareBuffer_describe(pixel_donor, &pixel_description);
    if (!describe_matches(&description, &pixel_description)) {
        LOGW("AHB cross-donor donor descriptions differ");
        goto out;
    }
    if (!trierarch_adreno_ahb_register_handle(&description, metadata_handle,
                pixel_handle->data[0], metadata_handle->data[1], &received))
        goto out;

    AHardwareBuffer_Desc received_description = {0};
    AHardwareBuffer_describe(received, &received_description);
    success = describe_matches(&description, &received_description);
    LOGI("AHB cross-donor: success=%d %ux%u stride=%u format=%u", success,
            received_description.width, received_description.height,
            received_description.stride, received_description.format);

out:
    if (received)
        AHardwareBuffer_release(received);
    if (pixel_donor)
        AHardwareBuffer_release(pixel_donor);
    if (metadata_donor)
        AHardwareBuffer_release(metadata_donor);
    if (!success)
        LOGW("AHB cross-donor failed");
    return success;
}
