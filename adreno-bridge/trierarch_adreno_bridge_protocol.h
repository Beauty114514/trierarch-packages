#ifndef TRIERARCH_ADRENO_BRIDGE_PROTOCOL_H
#define TRIERARCH_ADRENO_BRIDGE_PROTOCOL_H

#include <stdint.h>

/* Private, local ABI between Trierarch's Android host and its patched Mesa.
 * This is deliberately not a Wayland protocol and is never exposed to a
 * compositor or desktop client. */
#define TRIERARCH_ADRENO_BRIDGE_MAGIC 0x54524142u /* TRAB */
#define TRIERARCH_ADRENO_BRIDGE_VERSION 1u

enum trierarch_adreno_bridge_message_type {
    TRIERARCH_ADRENO_BRIDGE_HELLO = 1,
    TRIERARCH_ADRENO_BRIDGE_ALLOCATE = 2,
    TRIERARCH_ADRENO_BRIDGE_BUFFER = 3,
    TRIERARCH_ADRENO_BRIDGE_RELEASE = 4,
    TRIERARCH_ADRENO_BRIDGE_ERROR = 5,
};

struct trierarch_adreno_bridge_header {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
};

/* Mesa asks for a small swapchain matching one EGL Wayland surface. */
struct trierarch_adreno_bridge_allocate {
    struct trierarch_adreno_bridge_header header;
    uint32_t width;
    uint32_t height;
    uint32_t drm_format;
    uint32_t buffer_count;
};

/* Exactly one pixel-plane FD accompanies this message through SCM_RIGHTS. */
struct trierarch_adreno_bridge_buffer {
    struct trierarch_adreno_bridge_header header;
    uint32_t buffer_id;
    uint32_t width;
    uint32_t height;
    uint32_t drm_format;
    uint32_t stride;
    uint64_t modifier;
};

struct trierarch_adreno_bridge_release {
    struct trierarch_adreno_bridge_header header;
    uint32_t buffer_id;
};

struct trierarch_adreno_bridge_error {
    struct trierarch_adreno_bridge_header header;
    uint32_t code;
};

#endif
