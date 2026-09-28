#ifndef TRIERARCH_ADRENO_AHB_OFFSETS_H
#define TRIERARCH_ADRENO_AHB_OFFSETS_H

#include "adreno_ahb_layout.h"

#include <stddef.h>
#include <stdint.h>

void trierarch_adreno_ahb_find_offsets(const uint32_t *first,
        const uint32_t *second, const uint32_t *third, size_t words,
        uint32_t expected_first, uint32_t expected_second, uint32_t expected_third,
        struct trierarch_adreno_ahb_offsets *result);

const char *trierarch_adreno_ahb_format_offsets(
        const struct trierarch_adreno_ahb_offsets *offsets,
        char *buffer, size_t buffer_size);

#endif
