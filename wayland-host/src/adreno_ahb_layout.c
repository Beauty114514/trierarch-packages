#include "adreno_ahb_layout.h"
#include "adreno_ahb_native.h"
#include "adreno_ahb_offsets.h"

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define TAG "TrierarchAdrenoAhb"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

struct donor {
    AHardwareBuffer *buffer;
    const struct trierarch_adreno_native_handle *handle;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint64_t pixels_bytes;
    uint64_t metadata_bytes;
    const uint32_t *metadata;
};

static uint64_t fd_size(int fd) {
    off_t size = fd >= 0 ? lseek(fd, 0, SEEK_END) : -1;
    return size > 0 ? (uint64_t)size : 0;
}

static bool donor_create(struct donor *donor, uint32_t width, uint32_t height,
        uint32_t format, uint64_t usage) {
    const AHardwareBuffer_Desc requested = {
        .width = width,
        .height = height,
        .layers = 1,
        .format = format,
        .usage = usage,
    };
    if (AHardwareBuffer_allocate(&requested, &donor->buffer) != 0 || !donor->buffer)
        return false;

    AHardwareBuffer_Desc actual = {0};
    AHardwareBuffer_describe(donor->buffer, &actual);
    donor->handle = trierarch_adreno_ahb_native_handle(donor->buffer);
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

static void find_extent_offsets(struct trierarch_adreno_ahb_layout *result,
        const struct donor *first, const struct donor *second, const struct donor *third,
        const struct donor *validation) {
    const size_t words = (size_t)first->metadata_bytes / sizeof(uint32_t);
    for (size_t index = 0; index < words; ++index) {
        uint32_t values[] = {
            first->metadata[index], second->metadata[index], third->metadata[index],
        };
        uint64_t sizes[] = {
            first->pixels_bytes, second->pixels_bytes, third->pixels_bytes,
        };
        if (sizes[0] >= UINT32_MAX || values[0] <= sizes[0])
            continue;
        uint64_t delta = values[0] - sizes[0];
        if (!delta || delta > 0x100000 ||
                result->blob_extent.count == TRIERARCH_ADRENO_AHB_MAX_OFFSETS)
            continue;
        bool matches = true;
        for (unsigned sample = 1; sample < 3; ++sample)
            matches &= sizes[sample] < UINT32_MAX &&
                    values[sample] > sizes[sample] &&
                    values[sample] - sizes[sample] == delta;
        matches &= validation->pixels_bytes < UINT32_MAX &&
                validation->metadata[index] > validation->pixels_bytes &&
                validation->metadata[index] - validation->pixels_bytes == delta;
        if (!matches)
            continue;
        if (result->blob_extent.count && result->blob_extent_delta != delta)
            continue;
        result->blob_extent_delta = (uint32_t)delta;
        result->blob_extent.values[result->blob_extent.count++] =
                (uint32_t)(index * sizeof(uint32_t));
    }
}

static void donor_destroy(struct donor *donor) {
    if (donor->metadata)
        munmap((void *)donor->metadata, (size_t)donor->metadata_bytes);
    if (donor->buffer)
        AHardwareBuffer_release(donor->buffer);
}

static bool donor_layout_matches(const struct donor *reference,
        const struct donor *candidate) {
    return candidate->handle &&
            reference->handle->num_fds == candidate->handle->num_fds &&
            reference->handle->num_ints == candidate->handle->num_ints &&
            reference->metadata_bytes == candidate->metadata_bytes;
}

static void validate_extent_offsets(struct trierarch_adreno_ahb_layout *result,
        const struct donor *donor) {
    unsigned retained = 0;
    const size_t words = (size_t)donor->metadata_bytes / sizeof(uint32_t);

    for (unsigned index = 0; index < result->blob_extent.count; ++index) {
        uint32_t offset = result->blob_extent.values[index];
        if (offset % sizeof(uint32_t) || offset / sizeof(uint32_t) >= words ||
                donor->pixels_bytes >= UINT32_MAX)
            continue;
        uint32_t value = donor->metadata[offset / sizeof(uint32_t)];
        if (value > donor->pixels_bytes &&
                value - donor->pixels_bytes == result->blob_extent_delta)
            result->blob_extent.values[retained++] = offset;
    }
    result->blob_extent.count = retained;
}

/*
 * These checks only discard ambiguous offset candidates.  They do not infer
 * new metadata fields, and they never modify a donor or a guest dma-buf.
 */
static void validate_offsets_for_donor(struct trierarch_adreno_ahb_layout *result,
        const struct donor *donor) {
    const size_t blob_words = (size_t)donor->metadata_bytes / sizeof(uint32_t);
    const uint32_t *ints =
            (const uint32_t *)&donor->handle->data[donor->handle->num_fds];
    const size_t int_words = (size_t)donor->handle->num_ints;

#define VALIDATE_BLOB(field, expected) \
    trierarch_adreno_ahb_validate_offsets(&result->field, donor->metadata, \
            blob_words, (uint32_t)(expected))
#define VALIDATE_HANDLE(field, expected) \
    trierarch_adreno_ahb_validate_offsets(&result->field, ints, int_words, \
            (uint32_t)(expected))
    VALIDATE_BLOB(blob_width, donor->width);
    VALIDATE_BLOB(blob_height, donor->height);
    VALIDATE_BLOB(blob_stride_pixels, donor->stride);
    VALIDATE_BLOB(blob_stride_bytes, donor->stride * 4);
    VALIDATE_BLOB(blob_size, donor->pixels_bytes);
    VALIDATE_BLOB(blob_exact_size, donor->stride * donor->height * 4);
    VALIDATE_HANDLE(handle_width, donor->width);
    VALIDATE_HANDLE(handle_height, donor->height);
    VALIDATE_HANDLE(handle_stride_pixels, donor->stride);
    VALIDATE_HANDLE(handle_stride_bytes, donor->stride * 4);
    VALIDATE_HANDLE(handle_size, donor->pixels_bytes);
#undef VALIDATE_HANDLE
#undef VALIDATE_BLOB
    validate_extent_offsets(result, donor);
}

static void populate_offsets(struct trierarch_adreno_ahb_layout *result,
        const struct donor *first, const struct donor *second, const struct donor *third,
        const struct donor *validation) {
    size_t words = (size_t)first->metadata_bytes / sizeof(uint32_t);
    trierarch_adreno_ahb_find_offsets(first->metadata, second->metadata, third->metadata,
            words, first->width, second->width, third->width, &result->blob_width);
    trierarch_adreno_ahb_find_offsets(first->metadata, second->metadata, third->metadata,
            words, first->height, second->height, third->height, &result->blob_height);
    trierarch_adreno_ahb_find_offsets(first->metadata, second->metadata, third->metadata,
            words, first->stride, second->stride, third->stride, &result->blob_stride_pixels);
    trierarch_adreno_ahb_find_offsets(first->metadata, second->metadata, third->metadata,
            words, first->stride * 4, second->stride * 4, third->stride * 4,
            &result->blob_stride_bytes);
    trierarch_adreno_ahb_find_offsets(first->metadata, second->metadata, third->metadata,
            words, (uint32_t)first->pixels_bytes, (uint32_t)second->pixels_bytes,
            (uint32_t)third->pixels_bytes, &result->blob_size);
    trierarch_adreno_ahb_find_offsets(first->metadata, second->metadata, third->metadata,
            words, first->stride * first->height * 4,
            second->stride * second->height * 4, third->stride * third->height * 4,
            &result->blob_exact_size);
    find_extent_offsets(result, first, second, third, validation);

    const uint32_t *first_ints = (const uint32_t *)&first->handle->data[first->handle->num_fds];
    const uint32_t *second_ints = (const uint32_t *)&second->handle->data[second->handle->num_fds];
    const uint32_t *third_ints = (const uint32_t *)&third->handle->data[third->handle->num_fds];
    size_t ints = (size_t)first->handle->num_ints;
    trierarch_adreno_ahb_find_offsets(first_ints, second_ints, third_ints,
            ints, first->width, second->width, third->width, &result->handle_width);
    trierarch_adreno_ahb_find_offsets(first_ints, second_ints, third_ints,
            ints, first->height, second->height, third->height, &result->handle_height);
    trierarch_adreno_ahb_find_offsets(first_ints, second_ints, third_ints,
            ints, first->stride, second->stride, third->stride,
            &result->handle_stride_pixels);
    trierarch_adreno_ahb_find_offsets(first_ints, second_ints, third_ints,
            ints, first->stride * 4, second->stride * 4, third->stride * 4,
            &result->handle_stride_bytes);
    trierarch_adreno_ahb_find_offsets(first_ints, second_ints, third_ints,
            ints, (uint32_t)first->pixels_bytes, (uint32_t)second->pixels_bytes,
            (uint32_t)third->pixels_bytes, &result->handle_size);

    validate_offsets_for_donor(result, validation);
}

static void log_offsets(const struct trierarch_adreno_ahb_layout *layout) {
    char blob_height[64], blob_stride_pixels[64], blob_stride_bytes[64], blob_size[64];
    char handle_width[64], handle_height[64], handle_stride_pixels[64], handle_size[64];
    LOGI("AHB blob offsets: h=%s sp=%s sb=%s size=%s",
            trierarch_adreno_ahb_format_offsets(&layout->blob_height, blob_height,
                    sizeof(blob_height)),
            trierarch_adreno_ahb_format_offsets(&layout->blob_stride_pixels,
                    blob_stride_pixels, sizeof(blob_stride_pixels)),
            trierarch_adreno_ahb_format_offsets(&layout->blob_stride_bytes,
                    blob_stride_bytes, sizeof(blob_stride_bytes)),
            trierarch_adreno_ahb_format_offsets(&layout->blob_size, blob_size,
                    sizeof(blob_size)));
    LOGI("AHB handle offsets: w=%s h=%s sp=%s size=%s",
            trierarch_adreno_ahb_format_offsets(&layout->handle_width, handle_width,
                    sizeof(handle_width)),
            trierarch_adreno_ahb_format_offsets(&layout->handle_height, handle_height,
                    sizeof(handle_height)),
            trierarch_adreno_ahb_format_offsets(&layout->handle_stride_pixels,
                    handle_stride_pixels, sizeof(handle_stride_pixels)),
            trierarch_adreno_ahb_format_offsets(&layout->handle_size, handle_size,
                    sizeof(handle_size)));
}

bool trierarch_adreno_ahb_layout_calibrate_for_usage(uint32_t ahb_format,
        uint64_t usage,
        struct trierarch_adreno_ahb_layout *result) {
    if (!result)
        return false;
    memset(result, 0, sizeof(*result));
    if (!trierarch_adreno_ahb_native_handle_available()) {
        LOGW("AHardwareBuffer native-handle accessor is unavailable");
        return false;
    }

    /* These values deliberately change every relevant geometry field. */
    struct donor first = {0};
    struct donor second = {0};
    struct donor third = {0};
    struct donor validation = {0};
    bool valid = donor_create(&first, 300, 300, ahb_format, usage) &&
            donor_create(&second, 1134, 567, ahb_format, usage) &&
            donor_create(&third, 769, 127, ahb_format, usage) &&
            donor_create(&validation, 513, 947, ahb_format, usage);
    if (!valid || first.handle->num_ints != second.handle->num_ints ||
            first.handle->num_ints != third.handle->num_ints ||
            first.handle->num_ints != validation.handle->num_ints ||
            first.metadata_bytes != second.metadata_bytes ||
            first.metadata_bytes != third.metadata_bytes ||
            first.metadata_bytes != validation.metadata_bytes) {
        LOGW("AHB layout calibration donor mismatch");
        donor_destroy(&validation);
        donor_destroy(&third);
        donor_destroy(&second);
        donor_destroy(&first);
        return false;
    }

    result->donor_format = ahb_format;
    result->donor_usage = usage;
    result->native_handle_fds = first.handle->num_fds;
    result->native_handle_ints = first.handle->num_ints;
    result->metadata_bytes = first.metadata_bytes;
    result->first_stride = first.stride;
    result->second_stride = second.stride;
    populate_offsets(result, &first, &second, &third, &validation);
    result->validated_samples = 4;

    /*
     * The calibration donors above deliberately change both dimensions.  Add
     * orthogonal checks so a value that merely happens to correlate with the
     * original shapes cannot be labeled width, height, stride, or size.
     * 320/513/769 x 257 isolates width and stride changes; 513 x 127 is
     * paired with the existing 513 x 947 validation donor to isolate height.
     */
    const struct {
        uint32_t width;
        uint32_t height;
    } semantic_shapes[] = {
        {320, 257},
        {513, 257},
        {769, 257},
        {513, 127},
    };
    for (size_t index = 0; index < sizeof(semantic_shapes) / sizeof(semantic_shapes[0]);
            ++index) {
        struct donor semantic = {0};
        if (!donor_create(&semantic, semantic_shapes[index].width,
                semantic_shapes[index].height, ahb_format, usage) ||
                !donor_layout_matches(&first, &semantic)) {
            LOGW("AHB semantic donor mismatch at %ux%u",
                    semantic_shapes[index].width, semantic_shapes[index].height);
            donor_destroy(&semantic);
            donor_destroy(&validation);
            donor_destroy(&third);
            donor_destroy(&second);
            donor_destroy(&first);
            return false;
        }
        validate_offsets_for_donor(result, &semantic);
        ++result->validated_samples;
        donor_destroy(&semantic);
    }
    /* Vendor layouts do not duplicate every value.  On this device width is
     * represented in handle ints but not the blob; conversely, stride bytes
     * is represented in the blob but need not occupy a separate handle int.
     * These are the fields a future, separately reviewed forge needs to
     * safely patch; optional duplicates remain diagnostic only. */
    result->candidate = result->blob_height.count &&
            result->blob_stride_pixels.count && result->blob_stride_bytes.count &&
            result->blob_size.count && result->handle_width.count &&
            result->handle_height.count && result->handle_stride_pixels.count &&
            result->handle_size.count;
    result->all_fields_observed = result->candidate &&
            result->blob_exact_size.count && result->blob_extent.count &&
            result->handle_stride_bytes.count;
    LOGI("AHB layout calibration: samples=%u fmt=%u usage=0x%llx fds=%d ints=%d metadata=%llu "
            "candidate=%d all_fields=%d blob(w=%u h=%u sp=%u sb=%u size=%u exact=%u extent=%u+%u) "
            "ints(w=%u h=%u sp=%u sb=%u size=%u)",
            result->validated_samples, ahb_format, (unsigned long long)usage, result->native_handle_fds,
            result->native_handle_ints,
            (unsigned long long)result->metadata_bytes, result->candidate,
            result->all_fields_observed,
            result->blob_width.count, result->blob_height.count,
            result->blob_stride_pixels.count, result->blob_stride_bytes.count,
            result->blob_size.count, result->blob_exact_size.count,
            result->blob_extent.count, result->blob_extent_delta,
            result->handle_width.count, result->handle_height.count,
            result->handle_stride_pixels.count, result->handle_stride_bytes.count,
            result->handle_size.count);
    log_offsets(result);
    donor_destroy(&validation);
    donor_destroy(&third);
    donor_destroy(&second);
    donor_destroy(&first);
    return true;
}

bool trierarch_adreno_ahb_layout_calibrate(uint32_t ahb_format,
        struct trierarch_adreno_ahb_layout *result) {
    return trierarch_adreno_ahb_layout_calibrate_for_usage(ahb_format,
            AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, result);
}
