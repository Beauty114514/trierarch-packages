#ifndef TRIERARCH_WAYLAND_GPU_PROBE_H
#define TRIERARCH_WAYLAND_GPU_PROBE_H

#include <stdbool.h>
#include <stdint.h>
#include <android/hardware_buffer.h>

struct wayland_server;
struct trierarch_gpu_probe;

struct trierarch_gpu_probe_buffer {
    uint32_t width;
    uint32_t height;
    uint32_t drm_format;
    uint32_t stride;
    uint64_t modifier;
};

struct trierarch_gpu_probe *trierarch_gpu_probe_create(struct wayland_server *server,
        const char *runtime_dir);
void trierarch_gpu_probe_destroy(struct trierarch_gpu_probe *probe);
bool trierarch_gpu_probe_set_adreno_driver(struct trierarch_gpu_probe *probe,
        const char *hook_library_dir, const char *driver_dir, const char *driver_name);

AHardwareBuffer *trierarch_gpu_probe_copy_adreno(struct trierarch_gpu_probe *probe,
        const struct trierarch_gpu_probe_buffer *buffer, int buffer_fd);

#endif
