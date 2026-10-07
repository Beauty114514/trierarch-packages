#ifndef TRIERARCH_ADRENO_BRIDGE_CLIENT_H
#define TRIERARCH_ADRENO_BRIDGE_CLIENT_H

#include <stdint.h>

#include "trierarch_adreno_bridge_protocol.h"

#define TRIERARCH_ADRENO_BRIDGE_MAX_BUFFERS 4u

struct trierarch_adreno_bridge_client {
    int socket_fd;
    uint32_t buffer_count;
    struct trierarch_adreno_bridge_buffer buffers[
            TRIERARCH_ADRENO_BRIDGE_MAX_BUFFERS];
    int pixel_fds[TRIERARCH_ADRENO_BRIDGE_MAX_BUFFERS];
};

/* Opens a local bridge only when the caller supplies an explicit socket path.
 * It never falls back to a fixed Trierarch runtime directory. */
int trierarch_adreno_bridge_client_open(
        struct trierarch_adreno_bridge_client *client, const char *socket_path);

/* Allocates one surface-local host-owned swapchain. The returned FDs remain
 * owned by the client until close(); callers may duplicate them for DRI. */
int trierarch_adreno_bridge_client_allocate(
        struct trierarch_adreno_bridge_client *client, uint32_t width,
        uint32_t height, uint32_t drm_format, uint32_t buffer_count);

const struct trierarch_adreno_bridge_buffer *trierarch_adreno_bridge_client_buffer(
        const struct trierarch_adreno_bridge_client *client, uint32_t index,
        int *pixel_fd);

int trierarch_adreno_bridge_client_release(
        struct trierarch_adreno_bridge_client *client, uint32_t buffer_id);
void trierarch_adreno_bridge_client_close(
        struct trierarch_adreno_bridge_client *client);

#endif
