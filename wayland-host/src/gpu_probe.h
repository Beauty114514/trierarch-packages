#ifndef TRIERARCH_WAYLAND_GPU_PROBE_H
#define TRIERARCH_WAYLAND_GPU_PROBE_H

#include <stdbool.h>
#include <stdint.h>
#include <android/hardware_buffer.h>
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
/* Android owns this allocation until transfer.  The caller acquires its only
 * reference and must release it after sampling and reporting the result. */
bool trierarch_gpu_probe_take_host_buffer(struct trierarch_gpu_probe *probe,
        AHardwareBuffer **buffer, int *client_fd, int *guest_fence_fd);
/* Ownership of fence_fd transfers here. Pass -1 when no native fence exists. */
void trierarch_gpu_probe_report(int client_fd, uint32_t result, uint32_t egl_error, int fence_fd);

#endif
