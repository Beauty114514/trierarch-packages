#ifndef TRIERARCH_WAYLAND_DMABUF_FEEDBACK_DEVICE_H
#define TRIERARCH_WAYLAND_DMABUF_FEEDBACK_DEVICE_H

#include <stdbool.h>
#include <sys/types.h>

struct wayland_server;
struct trierarch_dmabuf_feedback_device;

struct trierarch_dmabuf_feedback_device *trierarch_dmabuf_feedback_device_create(
        struct wayland_server *server, const char *runtime_dir);
void trierarch_dmabuf_feedback_device_destroy(struct trierarch_dmabuf_feedback_device *device);
bool trierarch_dmabuf_feedback_device_get(const struct trierarch_dmabuf_feedback_device *device,
        dev_t *result);

#endif
