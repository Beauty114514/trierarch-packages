#include "adreno_ahb_guest.h"

#include "adreno_ahb_forge.h"
#include "adreno_ahb_native.h"
#include "adreno_ahb_wire.h"

#include <android/hardware_buffer.h>
#include <android/log.h>

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
    if (stride_bytes % 4)
        return false;
    const AHardwareBuffer_Desc target = {
        .width = width,
        .height = height,
        .stride = stride_bytes / 4,
        .layers = 1,
        .format = ahb_format,
        .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
    };
    /* The donor is metadata only.  Keep it tiny and patch its private
     * geometry before relaying it with the guest's pixel FD; reusing a
     * target-sized allocation leaves allocator-specific capacity state that
     * Android rejects when the guest allocation has a different tail. */
    const AHardwareBuffer_Desc donor_request = {
        .width = 4,
        .height = 4,
        .layers = 1,
        .format = ahb_format,
        .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
    };
    AHardwareBuffer *donor = NULL;
    AHardwareBuffer *received = NULL;
    struct trierarch_adreno_native_handle *prepared = NULL;
    bool success = false;
    if (AHardwareBuffer_allocate(&donor_request, &donor) != 0 || !donor)
        goto out;

    AHardwareBuffer_Desc description = {0};
    AHardwareBuffer_describe(donor, &description);
    const struct trierarch_adreno_native_handle *handle =
            trierarch_adreno_ahb_native_handle(donor);
    if (!handle || handle->num_fds != 2) {
        LOGW("AHB patched donor rejected handle");
        goto out;
    }
    uint64_t guest_bytes = fd_size(guest_fd);
    uint64_t donor_bytes = fd_size(handle->data[0]);
    uint64_t logical_bytes = (uint64_t)stride_bytes * height;
    bool geometry_matches = description.width == donor_request.width &&
            description.height == donor_request.height && donor_bytes > 0 &&
            guest_bytes >= logical_bytes;
    LOGI("AHB patched donor geometry: guest=%ux%u stride=%u logical=%llu bytes=%llu donor=%ux%u "
            "stride=%u bytes=%llu usable=%d", width, height, stride_bytes,
            (unsigned long long)logical_bytes,
            (unsigned long long)guest_bytes, description.width, description.height,
            description.stride * 4, (unsigned long long)donor_bytes,
            geometry_matches);
    if (!geometry_matches)
        goto out;

    /* The guest's KGSL allocation can include a page-alignment tail.  Relay
     * the whole pixel FD, but describe only the visible stride × height image
     * to gralloc; the tail is capacity, not image geometry. */
    if (!trierarch_adreno_ahb_forge_prepare(layout, handle, width, height,
                stride_bytes, logical_bytes, &prepared)) {
        LOGW("AHB full donor patch is unavailable for this allocator layout");
        goto out;
    }
    if (!trierarch_adreno_ahb_register_handle(&target, prepared, guest_fd,
                prepared->data[1], &received))
        goto out;

    AHardwareBuffer_Desc received_description = {0};
    AHardwareBuffer_describe(received, &received_description);
    success = describe_matches(&target, &received_description);
    LOGI("AHB patched donor import: success=%d %ux%u stride=%u format=%u",
            success, received_description.width, received_description.height,
            received_description.stride, received_description.format);
    if (success) {
        *buffer = received;
        received = NULL;
    }

out:
    trierarch_adreno_ahb_forge_destroy(prepared);
    if (received)
        AHardwareBuffer_release(received);
    if (donor)
        AHardwareBuffer_release(donor);
    if (!success)
        LOGW("AHB patched donor import failed");
    return success;
}
