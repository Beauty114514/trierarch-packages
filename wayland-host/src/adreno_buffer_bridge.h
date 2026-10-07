#ifndef TRIERARCH_ADRENO_BUFFER_BRIDGE_H
#define TRIERARCH_ADRENO_BUFFER_BRIDGE_H

#include <android/hardware_buffer.h>
#include <stdbool.h>
#include <stdint.h>

struct wayland_server;
struct trierarch_adreno_buffer_bridge;

struct trierarch_adreno_buffer_bridge *trierarch_adreno_buffer_bridge_create(
        struct wayland_server *server, const char *runtime_dir);
void trierarch_adreno_buffer_bridge_destroy(
        struct trierarch_adreno_buffer_bridge *bridge);

/* The compositor will use this in the next step to recognize a Mesa-submitted
 * FD and sample the original host allocation rather than a CPU mapping. */
AHardwareBuffer *trierarch_adreno_buffer_bridge_lookup(
        const struct trierarch_adreno_buffer_bridge *bridge, int dmabuf_fd,
        uint32_t *buffer_id);

#endif
