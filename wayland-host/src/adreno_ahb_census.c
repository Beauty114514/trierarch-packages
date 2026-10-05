#include "adreno_ahb_census.h"

#include "adreno_ahb_native.h"

#include <android/hardware_buffer.h>
#include <android/log.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>

#define TAG "TrierarchAdrenoAhb"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

enum {
    CENSUS_SAMPLES = 8,
    MAX_NATIVE_HANDLE_INTS = 64,
    MAX_UNKNOWN_WORDS = 32,
};

enum census_field {
    CENSUS_WIDTH,
    CENSUS_HEIGHT,
    CENSUS_STRIDE_PIXELS,
    CENSUS_STRIDE_BYTES,
    CENSUS_LOGICAL_BYTES,
    CENSUS_ALLOCATION_BYTES,
    CENSUS_ALLOCATION_ROWS,
    CENSUS_FIELD_COUNT,
};

struct census_sample {
    AHardwareBuffer *buffer;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint64_t pixels_bytes;
    uint64_t metadata_bytes;
    uint32_t ints[MAX_NATIVE_HANDLE_INTS];
    unsigned int_count;
    uint32_t *metadata;
};

static uint64_t fd_bytes(int fd) {
    struct stat status = {0};
    return fd >= 0 && fstat(fd, &status) == 0 && status.st_size > 0
            ? (uint64_t)status.st_size : 0;
}

static void census_sample_destroy(struct census_sample *sample) {
    free(sample->metadata);
    if (sample->buffer)
        AHardwareBuffer_release(sample->buffer);
    memset(sample, 0, sizeof(*sample));
}

static bool census_sample_create(struct census_sample *sample, uint32_t width,
        uint32_t height, uint32_t format, uint64_t usage) {
    const AHardwareBuffer_Desc request = {
        .width = width,
        .height = height,
        .layers = 1,
        .format = format,
        .usage = usage,
    };
    if (AHardwareBuffer_allocate(&request, &sample->buffer) != 0 || !sample->buffer)
        return false;

    AHardwareBuffer_Desc description = {0};
    AHardwareBuffer_describe(sample->buffer, &description);
    const struct trierarch_adreno_native_handle *handle =
            trierarch_adreno_ahb_native_handle(sample->buffer);
    if (!handle || handle->num_fds != 2 || handle->num_ints < 0 ||
            handle->num_ints > MAX_NATIVE_HANDLE_INTS)
        return false;
    sample->width = description.width;
    sample->height = description.height;
    sample->stride = description.stride;
    sample->pixels_bytes = fd_bytes(handle->data[0]);
    sample->metadata_bytes = fd_bytes(handle->data[1]);
    if (!sample->width || !sample->height || !sample->stride ||
            !sample->pixels_bytes || !sample->metadata_bytes ||
            sample->metadata_bytes > SIZE_MAX)
        return false;

    sample->metadata = malloc((size_t)sample->metadata_bytes);
    if (!sample->metadata)
        return false;
    void *mapped = mmap(NULL, (size_t)sample->metadata_bytes, PROT_READ,
            MAP_SHARED, handle->data[1], 0);
    if (mapped == MAP_FAILED) {
        LOGW("AHB census metadata mmap failed");
        return false;
    }
    memcpy(sample->metadata, mapped, (size_t)sample->metadata_bytes);
    munmap(mapped, (size_t)sample->metadata_bytes);
    sample->int_count = (unsigned)handle->num_ints;
    memcpy(sample->ints, &handle->data[handle->num_fds],
            sample->int_count * sizeof(*sample->ints));
    return true;
}

static bool census_field_value(const struct census_sample *sample,
        enum census_field field, uint32_t *result) {
    uint64_t logical_bytes = (uint64_t)sample->stride * sample->height * 4;
    uint64_t stride_bytes = (uint64_t)sample->stride * 4;
    uint64_t allocation_rows = stride_bytes ? sample->pixels_bytes / stride_bytes : 0;
    uint64_t value = 0;
    switch (field) {
    case CENSUS_WIDTH: value = sample->width; break;
    case CENSUS_HEIGHT: value = sample->height; break;
    case CENSUS_STRIDE_PIXELS: value = sample->stride; break;
    case CENSUS_STRIDE_BYTES: value = stride_bytes; break;
    case CENSUS_LOGICAL_BYTES: value = logical_bytes; break;
    case CENSUS_ALLOCATION_BYTES: value = sample->pixels_bytes; break;
    case CENSUS_ALLOCATION_ROWS:
        if (!stride_bytes || sample->pixels_bytes % stride_bytes)
            return false;
        value = allocation_rows;
        break;
    case CENSUS_FIELD_COUNT:
        return false;
    }
    if (value > UINT32_MAX)
        return false;
    *result = (uint32_t)value;
    return true;
}

static bool word_matches_field(const uint32_t *values,
        const struct census_sample samples[CENSUS_SAMPLES], enum census_field field) {
    for (unsigned sample = 0; sample < CENSUS_SAMPLES; ++sample) {
        uint32_t expected = 0;
        if (!census_field_value(&samples[sample], field, &expected) ||
                values[sample] != expected)
            return false;
    }
    return true;
}

static bool words_vary(const uint32_t *values) {
    for (unsigned sample = 1; sample < CENSUS_SAMPLES; ++sample)
        if (values[sample] != values[0])
            return true;
    return false;
}

static void format_values(const uint32_t *values, char *out, size_t out_size) {
    size_t used = 0;
    for (unsigned sample = 0; sample < CENSUS_SAMPLES && used < out_size; ++sample) {
        int written = snprintf(out + used, out_size - used, "%s%u",
                sample ? "," : "", values[sample]);
        if (written < 0 || (size_t)written >= out_size - used)
            break;
        used += (size_t)written;
    }
}

static void census_region(const char *name, const uint32_t *const words[CENSUS_SAMPLES],
        size_t word_count, const struct census_sample samples[CENSUS_SAMPLES]) {
    unsigned known[CENSUS_FIELD_COUNT] = {0};
    unsigned unknown = 0;
    unsigned logged_unknown = 0;
    for (size_t word = 0; word < word_count; ++word) {
        uint32_t values[CENSUS_SAMPLES] = {0};
        for (unsigned sample = 0; sample < CENSUS_SAMPLES; ++sample)
            values[sample] = words[sample][word];
        if (!words_vary(values))
            continue;
        bool recognized = false;
        for (unsigned field = 0; field < CENSUS_FIELD_COUNT; ++field) {
            if (word_matches_field(values, samples, (enum census_field)field)) {
                ++known[field];
                recognized = true;
            }
        }
        if (recognized)
            continue;
        ++unknown;
        if (logged_unknown++ < MAX_UNKNOWN_WORDS) {
            char value_text[192] = {0};
            format_values(values, value_text, sizeof(value_text));
            LOGI("AHB census %s unknown offset=0x%zx values=[%s]", name,
                    word * sizeof(uint32_t), value_text);
        }
    }
    LOGI("AHB census %s: known(w=%u h=%u sp=%u sb=%u logical=%u alloc=%u rows=%u) "
            "unknown-changing=%u logged=%u", name,
            known[CENSUS_WIDTH], known[CENSUS_HEIGHT],
            known[CENSUS_STRIDE_PIXELS], known[CENSUS_STRIDE_BYTES],
            known[CENSUS_LOGICAL_BYTES], known[CENSUS_ALLOCATION_BYTES],
            known[CENSUS_ALLOCATION_ROWS], unknown,
            unknown < MAX_UNKNOWN_WORDS ? unknown : MAX_UNKNOWN_WORDS);
}

void trierarch_adreno_ahb_census_run(uint32_t ahb_format, uint64_t usage) {
    static const struct { uint32_t width, height; } shapes[CENSUS_SAMPLES] = {
        {300, 300}, {1134, 567}, {769, 127}, {513, 947},
        {320, 257}, {513, 257}, {769, 257}, {513, 127},
    };
    struct census_sample samples[CENSUS_SAMPLES] = {0};
    bool valid = trierarch_adreno_ahb_native_handle_available();
    for (unsigned sample = 0; valid && sample < CENSUS_SAMPLES; ++sample)
        valid = census_sample_create(&samples[sample], shapes[sample].width,
                shapes[sample].height, ahb_format, usage);
    if (!valid) {
        LOGW("AHB census could not capture all donor samples");
        goto out;
    }
    for (unsigned sample = 1; sample < CENSUS_SAMPLES; ++sample) {
        if (samples[sample].metadata_bytes != samples[0].metadata_bytes ||
                samples[sample].int_count != samples[0].int_count) {
            LOGW("AHB census donor layout drift at sample=%u", sample);
            goto out;
        }
    }

    const uint32_t *metadata[CENSUS_SAMPLES];
    const uint32_t *ints[CENSUS_SAMPLES];
    for (unsigned sample = 0; sample < CENSUS_SAMPLES; ++sample) {
        metadata[sample] = samples[sample].metadata;
        ints[sample] = samples[sample].ints;
    }
    LOGI("AHB census begin: samples=%u fmt=%u usage=0x%llx metadata=%llu ints=%u",
            CENSUS_SAMPLES, ahb_format, (unsigned long long)usage,
            (unsigned long long)samples[0].metadata_bytes, samples[0].int_count);
    for (unsigned sample = 0; sample < CENSUS_SAMPLES; ++sample) {
        uint64_t stride_bytes = (uint64_t)samples[sample].stride * 4;
        uint64_t logical_bytes = stride_bytes * samples[sample].height;
        uint64_t allocation_rows = stride_bytes
                ? samples[sample].pixels_bytes / stride_bytes : 0;
        LOGI("AHB census sample=%u geometry=%ux%u stride=%u logical=%llu allocation=%llu "
                "rows=%llu", sample, samples[sample].width, samples[sample].height,
                samples[sample].stride, (unsigned long long)logical_bytes,
                (unsigned long long)samples[sample].pixels_bytes,
                (unsigned long long)allocation_rows);
    }
    census_region("blob", metadata,
            (size_t)samples[0].metadata_bytes / sizeof(uint32_t), samples);
    census_region("ints", ints, samples[0].int_count, samples);

out:
    for (unsigned sample = 0; sample < CENSUS_SAMPLES; ++sample)
        census_sample_destroy(&samples[sample]);
}
