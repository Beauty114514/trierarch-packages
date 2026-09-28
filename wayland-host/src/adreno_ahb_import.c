#include "adreno_ahb_import.h"

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <dlfcn.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define TAG "TrierarchAdrenoAhb"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

#define DRM_FORMAT_ABGR8888 0x34324241u
#define DRM_FORMAT_XRGB8888 0x34325258u

/* VNDK exposes this through libnativewindow, but it is not an NDK API. */
struct native_handle {
    int version;
    int num_fds;
    int num_ints;
    int data[];
};

typedef const struct native_handle *(*get_native_handle_fn)(const AHardwareBuffer *);

struct donor_layout {
    uint32_t stride;
    int fds;
    int ints;
    uint64_t metadata_bytes;
};

static get_native_handle_fn resolve_native_handle(void) {
    static bool attempted;
    static get_native_handle_fn function;
    if (attempted)
        return function;
    attempted = true;
    function = (get_native_handle_fn)dlsym(RTLD_DEFAULT, "AHardwareBuffer_getNativeHandle");
    if (function)
        return function;
    void *library = dlopen("libnativewindow.so", RTLD_NOW | RTLD_LOCAL);
    if (library)
        function = (get_native_handle_fn)dlsym(library, "AHardwareBuffer_getNativeHandle");
    return function;
}

static uint64_t fd_size(int fd) {
    off_t size = fd >= 0 ? lseek(fd, 0, SEEK_END) : -1;
    return size > 0 ? (uint64_t)size : 0;
}

static bool inspect_donor(uint32_t width, uint32_t height, uint32_t format,
        get_native_handle_fn native_handle, struct donor_layout *layout) {
    const AHardwareBuffer_Desc requested = {
        .width = width,
        .height = height,
        .layers = 1,
        .format = format,
        .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
    };
    AHardwareBuffer *buffer = NULL;
    if (AHardwareBuffer_allocate(&requested, &buffer) != 0 || !buffer)
        return false;

    AHardwareBuffer_Desc actual = {0};
    AHardwareBuffer_describe(buffer, &actual);
    const struct native_handle *handle = native_handle(buffer);
    bool valid = handle && handle->num_fds >= 1 && handle->num_ints >= 0;
    if (valid) {
        layout->stride = actual.stride;
        layout->fds = handle->num_fds;
        layout->ints = handle->num_ints;
        /* Anland's QCOM path identifies a donor metadata blob at fd[1]. */
        layout->metadata_bytes = handle->num_fds == 2 ? fd_size(handle->data[1]) : 0;
    }
    AHardwareBuffer_release(buffer);
    return valid;
}

bool trierarch_adreno_ahb_preflight_run(uint32_t width, uint32_t height,
        uint32_t drm_format, struct trierarch_adreno_ahb_preflight *result) {
    if (!result || !width || !height)
        return false;
    memset(result, 0, sizeof(*result));

    get_native_handle_fn native_handle = resolve_native_handle();
    if (!native_handle) {
        LOGW("AHardwareBuffer native-handle accessor is unavailable");
        return false;
    }

    /* The probe clients use one-plane 32-bit RGB buffers.  Pixel conversion
     * is not being tested yet; this stage only examines Android donor layout. */
    if (drm_format != DRM_FORMAT_ABGR8888 && drm_format != DRM_FORMAT_XRGB8888) {
        LOGI("preflight skipped for unsupported DRM format=0x%x", drm_format);
        return false;
    }

    struct donor_layout first = {0};
    struct donor_layout second = {0};
    uint32_t alternate_width = width == UINT32_MAX ? width - 1 : width + 1;
    uint32_t alternate_height = height == UINT32_MAX ? height - 1 : height + 1;
    if (!alternate_width || !alternate_height ||
            !inspect_donor(width, height, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
                    native_handle, &first) ||
            !inspect_donor(alternate_width, alternate_height,
                    AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, native_handle, &second)) {
        LOGW("AHardwareBuffer donor allocation or inspection failed");
        return false;
    }

    result->donor_format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    result->first_stride = first.stride;
    result->second_stride = second.stride;
    result->native_handle_fds = first.fds;
    result->native_handle_ints = first.ints;
    result->metadata_bytes = first.metadata_bytes;
    result->candidate = first.fds == 2 && second.fds == 2 &&
            first.ints == second.ints && first.metadata_bytes > 0 &&
            first.metadata_bytes == second.metadata_bytes;

    LOGI("AHB donor preflight: request=%ux%u alternate=%ux%u "
            "strides=%u/%u fds=%d/%d ints=%d/%d metadata=%llu/%llu candidate=%d",
            width, height, alternate_width, alternate_height,
            first.stride, second.stride, first.fds, second.fds, first.ints, second.ints,
            (unsigned long long)first.metadata_bytes,
            (unsigned long long)second.metadata_bytes, result->candidate);
    return true;
}
