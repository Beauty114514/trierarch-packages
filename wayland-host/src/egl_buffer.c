#include "server_internal.h"

#include <stdlib.h>

static void egl_resource_destroyed(struct wl_listener *listener, void *data) {
    (void)data;
    struct shm_buffer *buffer =
            wl_container_of(listener, buffer, egl_resource_destroy_listener);
    buffer->egl_resource_destroy_listener_attached = false;
    trierarch_renderer_wait_idle(buffer->server ? buffer->server->renderer : NULL);
    buffer->egl_resource = NULL;
    buffer->resource = NULL;
}

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
    buffer->destroy = trierarch_buffer_destroy_storage;
    buffer->server = server;
    buffer->acquire_fence_fd = -1;
    buffer->egl_resource = resource;
    buffer->egl_resource_destroy_listener.notify = egl_resource_destroyed;
    wl_resource_add_destroy_listener(resource,
            &buffer->egl_resource_destroy_listener);
    buffer->egl_resource_destroy_listener_attached = true;
    buffer->width = server->output_width > 0 ? server->output_width : 1080;
    buffer->height = server->output_height > 0 ? server->output_height : 1920;
    buffer->stride = buffer->width * 4;
    buffer->format = WL_SHM_FORMAT_XRGB8888;
    return buffer;
}
