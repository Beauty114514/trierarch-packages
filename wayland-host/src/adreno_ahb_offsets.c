#include "adreno_ahb_offsets.h"

#include <stdio.h>
#include <string.h>

void trierarch_adreno_ahb_find_offsets(const uint32_t *first,
        const uint32_t *second, const uint32_t *third, size_t words,
        uint32_t expected_first, uint32_t expected_second, uint32_t expected_third,
        struct trierarch_adreno_ahb_offsets *result) {
    memset(result, 0, sizeof(*result));
    if (expected_first == expected_second || expected_first == expected_third ||
            expected_second == expected_third)
        return;
    for (size_t index = 0; index < words &&
            result->count < TRIERARCH_ADRENO_AHB_MAX_OFFSETS; ++index) {
        if (first[index] == expected_first && second[index] == expected_second &&
                third[index] == expected_third)
            result->values[result->count++] = (uint32_t)(index * sizeof(uint32_t));
    }
}

const char *trierarch_adreno_ahb_format_offsets(
        const struct trierarch_adreno_ahb_offsets *offsets,
        char *buffer, size_t buffer_size) {
    if (!buffer_size)
        return buffer;
    size_t used = 0;
    int written = snprintf(buffer, buffer_size, "[");
    if (written < 0)
        return buffer;
    used = (size_t)written;
    for (unsigned index = 0; index < offsets->count && used < buffer_size; ++index) {
        written = snprintf(buffer + used, buffer_size - used, "%s0x%x",
                index ? "," : "", offsets->values[index]);
        if (written < 0)
            return buffer;
        used += (size_t)written;
    }
    if (used < buffer_size)
        snprintf(buffer + used, buffer_size - used, "]");
    return buffer;
}
