#ifndef TRIERARCH_WAYLAND_GPU_PROBE_H
#define TRIERARCH_WAYLAND_GPU_PROBE_H

#include <stdbool.h>
#include <stdint.h>
#include "protocol.h"

struct wayland_server;
struct trierarch_gpu_probe;

struct trierarch_gpu_probe *trierarch_gpu_probe_create(struct wayland_server *server,
        const char *runtime_dir);
void trierarch_gpu_probe_destroy(struct trierarch_gpu_probe *probe);

/* Called only by the Wayland dispatch/render thread.  Ownership of returned
 * FDs moves to the caller, which must report exactly once. */
bool trierarch_gpu_probe_take(struct trierarch_gpu_probe *probe,
        struct trierarch_gpu_probe_buffer *buffer, int *buffer_fd, int *client_fd);
void trierarch_gpu_probe_report(int client_fd, uint32_t result, uint32_t egl_error);

#endif
