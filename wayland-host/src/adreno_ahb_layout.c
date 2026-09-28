#include "adreno_ahb_layout.h"

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define TAG "TrierarchAdrenoAhb"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

/* VNDK exposes this through libnativewindow, but it is not an NDK API. */
struct native_handle {
    int version;
    int num_fds;
    int num_ints;
    int data[];
};

typedef const struct native_handle *(*get_native_handle_fn)(const AHardwareBuffer *);

struct donor {
    AHardwareBuffer *buffer;
    const struct native_handle *handle;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint64_t pixels_bytes;
    uint64_t metadata_bytes;
    const uint32_t *metadata;
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

static bool donor_create(struct donor *donor, uint32_t width, uint32_t height,
        uint32_t format, get_native_handle_fn native_handle) {
    const AHardwareBuffer_Desc requested = {
        .width = width,
        .height = height,
        .layers = 1,
        .format = format,
        .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
    };
    if (AHardwareBuffer_allocate(&requested, &donor->buffer) != 0 || !donor->buffer)
        return false;

    AHardwareBuffer_Desc actual = {0};
    AHardwareBuffer_describe(donor->buffer, &actual);
    donor->handle = native_handle(donor->buffer);
    if (!donor->handle || donor->handle->num_fds != 2 || donor->handle->num_ints < 0)
        return false;
    donor->width = width;
    donor->height = height;
    donor->stride = actual.stride;
    donor->pixels_bytes = fd_size(donor->handle->data[0]);
    donor->metadata_bytes = fd_size(donor->handle->data[1]);
    if (!donor->pixels_bytes || !donor->metadata_bytes || donor->metadata_bytes > SIZE_MAX)
        return false;
    void *mapped = mmap(NULL, (size_t)donor->metadata_bytes, PROT_READ,
            MAP_SHARED, donor->handle->data[1], 0);
    if (mapped == MAP_FAILED) {
        LOGW("AHB donor metadata mmap failed: %s", strerror(errno));
        return false;
    }
    donor->metadata = mapped;
    return true;
}

static void donor_destroy(struct donor *donor) {
    if (donor->metadata)
        munmap((void *)donor->metadata, (size_t)donor->metadata_bytes);
    if (donor->buffer)
        AHardwareBuffer_release(donor->buffer);
}

static unsigned count_value_pairs(const uint32_t *first, const uint32_t *second,
        size_t words, uint32_t expected_first, uint32_t expected_second) {
    unsigned matches = 0;
    for (size_t index = 0; index < words; ++index) {
        if (first[index] == expected_first && second[index] == expected_second &&
                expected_first != expected_second)
            ++matches;
    }
    return matches;
}

static void populate_matches(struct trierarch_adreno_ahb_layout *result,
        const struct donor *first, const struct donor *second) {
    size_t words = (size_t)first->metadata_bytes / sizeof(uint32_t);
    result->blob_width_matches = count_value_pairs(first->metadata, second->metadata,
            words, first->width, second->width);
    result->blob_height_matches = count_value_pairs(first->metadata, second->metadata,
            words, first->height, second->height);
    result->blob_stride_pixels_matches = count_value_pairs(first->metadata, second->metadata,
            words, first->stride, second->stride);
    result->blob_stride_bytes_matches = count_value_pairs(first->metadata, second->metadata,
            words, first->stride * 4, second->stride * 4);
    result->blob_size_matches = count_value_pairs(first->metadata, second->metadata,
            words, (uint32_t)first->pixels_bytes, (uint32_t)second->pixels_bytes);
    result->blob_exact_size_matches = count_value_pairs(first->metadata, second->metadata,
            words, first->stride * first->height * 4,
            second->stride * second->height * 4);

    const uint32_t *first_ints = (const uint32_t *)&first->handle->data[first->handle->num_fds];
    const uint32_t *second_ints = (const uint32_t *)&second->handle->data[second->handle->num_fds];
    size_t ints = (size_t)first->handle->num_ints;
    result->handle_width_matches = count_value_pairs(first_ints, second_ints,
            ints, first->width, second->width);
    result->handle_height_matches = count_value_pairs(first_ints, second_ints,
            ints, first->height, second->height);
    result->handle_stride_pixels_matches = count_value_pairs(first_ints, second_ints,
            ints, first->stride, second->stride);
    result->handle_stride_bytes_matches = count_value_pairs(first_ints, second_ints,
            ints, first->stride * 4, second->stride * 4);
    result->handle_size_matches = count_value_pairs(first_ints, second_ints,
            ints, (uint32_t)first->pixels_bytes, (uint32_t)second->pixels_bytes);
}

bool trierarch_adreno_ahb_layout_calibrate(uint32_t ahb_format,
        struct trierarch_adreno_ahb_layout *result) {
    if (!result)
        return false;
    memset(result, 0, sizeof(*result));
    get_native_handle_fn native_handle = resolve_native_handle();
    if (!native_handle) {
        LOGW("AHardwareBuffer native-handle accessor is unavailable");
        return false;
    }

    /* These values deliberately change every relevant geometry field. */
    struct donor first = {0};
    struct donor second = {0};
    bool valid = donor_create(&first, 300, 300, ahb_format, native_handle) &&
            donor_create(&second, 1134, 567, ahb_format, native_handle);
    if (!valid || first.handle->num_ints != second.handle->num_ints ||
            first.metadata_bytes != second.metadata_bytes) {
        LOGW("AHB layout calibration donor mismatch");
        donor_destroy(&second);
        donor_destroy(&first);
        return false;
    }

    result->donor_format = ahb_format;
    result->native_handle_fds = first.handle->num_fds;
    result->native_handle_ints = first.handle->num_ints;
    result->metadata_bytes = first.metadata_bytes;
    result->first_stride = first.stride;
    result->second_stride = second.stride;
    populate_matches(result, &first, &second);
    /* Vendor layouts do not duplicate every value.  On this device width is
     * represented in handle ints but not the blob; conversely, stride bytes
     * is represented in the blob but need not occupy a separate handle int.
     * These are the fields a future, separately reviewed forge needs to
     * safely patch; optional duplicates remain diagnostic only. */
    result->candidate = result->blob_height_matches &&
            result->blob_stride_pixels_matches && result->blob_stride_bytes_matches &&
            result->blob_size_matches && result->blob_exact_size_matches &&
            result->handle_width_matches && result->handle_height_matches &&
            result->handle_stride_pixels_matches && result->handle_size_matches;
    LOGI("AHB layout calibration: fmt=%u fds=%d ints=%d metadata=%llu candidate=%d "
            "blob(w=%u h=%u sp=%u sb=%u size=%u exact=%u) "
            "ints(w=%u h=%u sp=%u sb=%u size=%u)",
            ahb_format, result->native_handle_fds, result->native_handle_ints,
            (unsigned long long)result->metadata_bytes, result->candidate,
            result->blob_width_matches, result->blob_height_matches,
            result->blob_stride_pixels_matches, result->blob_stride_bytes_matches,
            result->blob_size_matches, result->blob_exact_size_matches,
            result->handle_width_matches,
            result->handle_height_matches, result->handle_stride_pixels_matches,
            result->handle_stride_bytes_matches, result->handle_size_matches);
    donor_destroy(&second);
    donor_destroy(&first);
    return true;
}
