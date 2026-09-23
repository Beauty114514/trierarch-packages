#ifndef TRIERARCH_GPU_PROBE_PROTOCOL_H
#define TRIERARCH_GPU_PROBE_PROTOCOL_H

#include <stdint.h>

#define TRIERARCH_GPU_PROBE_MAGIC 0x54524750u /* TRGP */
#define TRIERARCH_GPU_PROBE_VERSION 3u

enum trierarch_gpu_probe_message_type {
    TRIERARCH_GPU_PROBE_HELLO = 1,
    TRIERARCH_GPU_PROBE_GUEST_BUFFER = 2,
    TRIERARCH_GPU_PROBE_HOST_BUFFER = 3,
    TRIERARCH_GPU_PROBE_RESULT = 4,
};

enum trierarch_gpu_probe_direction {
    TRIERARCH_GPU_PROBE_GUEST_TO_HOST = 1,
    TRIERARCH_GPU_PROBE_HOST_TO_GUEST = 2,
};

enum trierarch_gpu_probe_result {
    TRIERARCH_GPU_PROBE_OK = 0,
    TRIERARCH_GPU_PROBE_BAD_MESSAGE = 1,
    TRIERARCH_GPU_PROBE_IMPORT_UNAVAILABLE = 2,
    TRIERARCH_GPU_PROBE_IMPORT_FAILED = 3,
    TRIERARCH_GPU_PROBE_DRAW_FAILED = 4,
};

struct trierarch_gpu_probe_hello {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t direction;
};

/* Exactly one FD accompanies a BUFFER.  This deliberately tests only a
 * single-plane Android allocation; a native handle with private metadata is
 * not a general DRM dma-buf protocol. */
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

/* RESULT may carry exactly one optional SCM_RIGHTS FD.  In the reverse probe
 * it is a native sync_file fence: guest->host protects the guest write; the
 * host->guest reply protects the host sample before buffer reuse. */

#endif
