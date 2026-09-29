#include "adreno_ahb_patch.h"

#include <android/log.h>

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define TAG "TrierarchAdrenoAhb"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

static bool offsets_fit(const struct trierarch_adreno_ahb_offsets *offsets,
        size_t byte_limit) {
    if (!offsets || !offsets->count || byte_limit < sizeof(uint32_t))
        return false;
    for (unsigned index = 0; index < offsets->count; ++index) {
        uint32_t offset = offsets->values[index];
        if (offset % sizeof(uint32_t) || offset > byte_limit - sizeof(uint32_t))
            return false;
    }
    return true;
}

static void patch_words(uint32_t *words,
        const struct trierarch_adreno_ahb_offsets *offsets, uint32_t value) {
    for (unsigned index = 0; index < offsets->count; ++index)
        words[offsets->values[index] / sizeof(uint32_t)] = value;
}

bool trierarch_adreno_ahb_patch_allocation_size(
        const struct trierarch_adreno_ahb_layout *layout,
        const struct trierarch_adreno_native_handle *donor,
        uint64_t guest_bytes, struct trierarch_adreno_native_handle **patched) {
    if (!layout || !donor || !patched || !layout->candidate ||
            guest_bytes > UINT32_MAX || donor->num_fds != 2 || donor->num_ints < 0 ||
            donor->num_fds != layout->native_handle_fds ||
            donor->num_ints != layout->native_handle_ints ||
            !layout->metadata_bytes || layout->metadata_bytes > SIZE_MAX ||
            !offsets_fit(&layout->blob_size, (size_t)layout->metadata_bytes) ||
            !offsets_fit(&layout->handle_size, (size_t)donor->num_ints * sizeof(uint32_t)))
        return false;

    *patched = NULL;
    size_t values = (size_t)donor->num_fds + (size_t)donor->num_ints;
    if (values > (SIZE_MAX - offsetof(struct trierarch_adreno_native_handle, data)) /
            sizeof(donor->data[0]))
        return false;
    size_t bytes = offsetof(struct trierarch_adreno_native_handle, data) +
            values * sizeof(donor->data[0]);
    struct trierarch_adreno_native_handle *copy = malloc(bytes);
    if (!copy)
        return false;
    memcpy(copy, donor, bytes);

    void *mapping = mmap(NULL, (size_t)layout->metadata_bytes, PROT_READ | PROT_WRITE,
            MAP_SHARED, donor->data[1], 0);
    if (mapping == MAP_FAILED) {
        LOGW("AHB donor metadata writable mmap failed: %s", strerror(errno));
        free(copy);
        return false;
    }
    patch_words(mapping, &layout->blob_size, (uint32_t)guest_bytes);
    uint32_t *handle_ints = (uint32_t *)&copy->data[copy->num_fds];
    patch_words(handle_ints, &layout->handle_size, (uint32_t)guest_bytes);
    munmap(mapping, (size_t)layout->metadata_bytes);

    LOGI("AHB patched transient donor allocation: bytes=%llu blob-fields=%u handle-fields=%u",
            (unsigned long long)guest_bytes, layout->blob_size.count,
            layout->handle_size.count);
    *patched = copy;
    return true;
}
