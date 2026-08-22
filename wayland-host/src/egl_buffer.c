#include "server_internal.h"

#include <stdlib.h>

/* The EGL Wayland extension owns the actual wl_buffer implementation. We keep
 * a lightweight compositor-side wrapper so the renderer can identify it and
 * pass the original resource to eglCreateImageKHR(EGL_WAYLAND_BUFFER_WL). */
struct shm_buffer *trierarch_egl_buffer_from_resource(struct wl_resource *resource,
        struct wayland_server *server) {
    if (!resource || !server || !server->egl_buffer_supported)
        return NULL;
    struct shm_buffer *buffer = calloc(1, sizeof(*buffer));
    if (!buffer) return NULL;
    buffer->resource = resource;
    buffer->egl_buffer = true;
    buffer->egl_resource = resource;
    buffer->width = server->output_width > 0 ? server->output_width : 1080;
    buffer->height = server->output_height > 0 ? server->output_height : 1920;
    buffer->stride = buffer->width * 4;
    buffer->format = WL_SHM_FORMAT_XRGB8888;
    return buffer;
}
