#include "adreno_ahb_forge.h"

#include <android/log.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define TAG "TrierarchAdrenoAhb"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

static bool offsets_valid(const struct trierarch_adreno_ahb_offsets *offsets,
        size_t words) {
    if (!offsets || !offsets->count)
        return false;
    for (unsigned index = 0; index < offsets->count; ++index) {
        if (offsets->values[index] % sizeof(uint32_t) ||
                offsets->values[index] / sizeof(uint32_t) >= words)
            return false;
    }
    return true;
}

static void patch_words(uint32_t *words,
        const struct trierarch_adreno_ahb_offsets *offsets, uint32_t value) {
    for (unsigned index = 0; index < offsets->count; ++index)
        words[offsets->values[index] / sizeof(uint32_t)] = value;
}

static bool layout_can_forge(const struct trierarch_adreno_ahb_layout *layout,
        const struct trierarch_adreno_native_handle *donor) {
    if (!layout || !donor || donor->num_fds != 2 || donor->num_ints < 0 ||
            donor->num_ints != layout->native_handle_ints || !layout->metadata_bytes)
        return false;
    const size_t metadata_words = (size_t)layout->metadata_bytes / sizeof(uint32_t);
    return offsets_valid(&layout->blob_height, metadata_words) &&
            offsets_valid(&layout->blob_stride_pixels, metadata_words) &&
            offsets_valid(&layout->blob_stride_bytes, metadata_words) &&
            offsets_valid(&layout->blob_size, metadata_words) &&
            offsets_valid(&layout->blob_extent, metadata_words) &&
            offsets_valid(&layout->handle_width, (size_t)donor->num_ints) &&
            offsets_valid(&layout->handle_height, (size_t)donor->num_ints) &&
            offsets_valid(&layout->handle_stride_pixels, (size_t)donor->num_ints) &&
            offsets_valid(&layout->handle_size, (size_t)donor->num_ints);
}

bool trierarch_adreno_ahb_forge_prepare(
        const struct trierarch_adreno_ahb_layout *layout,
        const struct trierarch_adreno_native_handle *donor,
        uint32_t width, uint32_t height, uint32_t stride_bytes,
        uint64_t allocation_bytes,
        struct trierarch_adreno_native_handle **prepared) {
    if (!prepared || !layout || !width || !height || !stride_bytes || stride_bytes % 4 ||
            allocation_bytes > UINT32_MAX || !layout_can_forge(layout, donor) ||
            allocation_bytes + layout->blob_extent_delta > UINT32_MAX)
        return false;
    *prepared = NULL;

    const size_t metadata_bytes = (size_t)layout->metadata_bytes;
    uint32_t *metadata = mmap(NULL, metadata_bytes, PROT_READ | PROT_WRITE,
            MAP_SHARED, donor->data[1], 0);
    if (metadata == MAP_FAILED) {
        LOGW("AHB full donor patch metadata mmap failed");
        return false;
    }
    patch_words(metadata, &layout->blob_height, height);
    patch_words(metadata, &layout->blob_stride_pixels, stride_bytes / 4);
    patch_words(metadata, &layout->blob_stride_bytes, stride_bytes);
    patch_words(metadata, &layout->blob_size, (uint32_t)allocation_bytes);
    patch_words(metadata, &layout->blob_extent,
            (uint32_t)(allocation_bytes + layout->blob_extent_delta));
    munmap(metadata, metadata_bytes);

    const size_t data_count = (size_t)donor->num_fds + donor->num_ints;
    struct trierarch_adreno_native_handle *copy =
            calloc(1, sizeof(*copy) + data_count * sizeof(*copy->data));
    if (!copy)
        return false;
    copy->version = donor->version;
    copy->num_fds = donor->num_fds;
    copy->num_ints = donor->num_ints;
    memcpy(copy->data, donor->data, data_count * sizeof(*copy->data));
    uint32_t *ints = (uint32_t *)&copy->data[copy->num_fds];
    patch_words(ints, &layout->handle_width, width);
    patch_words(ints, &layout->handle_height, height);
    patch_words(ints, &layout->handle_stride_pixels, stride_bytes / 4);
    patch_words(ints, &layout->handle_size, (uint32_t)allocation_bytes);
    LOGI("AHB full donor patch: %ux%u stride=%u allocation=%llu extent=%llu",
            width, height, stride_bytes, (unsigned long long)allocation_bytes,
            (unsigned long long)(allocation_bytes + layout->blob_extent_delta));
    *prepared = copy;
    return true;
}

void trierarch_adreno_ahb_forge_destroy(
        struct trierarch_adreno_native_handle *prepared) {
    free(prepared);
}
