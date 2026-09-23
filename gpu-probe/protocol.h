#ifndef TRIERARCH_GPU_PROBE_PROTOCOL_H
#define TRIERARCH_GPU_PROBE_PROTOCOL_H

#include <stdint.h>

#define TRIERARCH_GPU_PROBE_MAGIC 0x54524750u /* TRGP */
#define TRIERARCH_GPU_PROBE_VERSION 1u

enum trierarch_gpu_probe_message_type {
    TRIERARCH_GPU_PROBE_BUFFER = 1,
    TRIERARCH_GPU_PROBE_RESULT = 2,
};

enum trierarch_gpu_probe_result {
    TRIERARCH_GPU_PROBE_OK = 0,
    TRIERARCH_GPU_PROBE_BAD_MESSAGE = 1,
    TRIERARCH_GPU_PROBE_IMPORT_UNAVAILABLE = 2,
    TRIERARCH_GPU_PROBE_IMPORT_FAILED = 3,
    TRIERARCH_GPU_PROBE_DRAW_FAILED = 4,
};

/* Exactly one FD accompanies BUFFER.  This restriction is intentional for
 * the first feasibility probe; it avoids pretending Android native handles
 * are equivalent to arbitrary multi-plane DRM buffers. */
struct trierarch_gpu_probe_buffer {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t width;
    uint32_t height;
    uint32_t drm_format;
    uint32_t stride;
    uint64_t modifier;
};

struct trierarch_gpu_probe_result_message {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t result;
    uint32_t egl_error;
};

#endif
