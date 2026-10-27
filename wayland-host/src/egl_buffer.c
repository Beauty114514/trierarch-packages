#include "server_internal.h"

#include <stdlib.h>

static void apply_output_size(struct shm_buffer *buffer, struct wayland_server *server) {
    buffer->width = server->output_width > 0 ? server->output_width : 1080;
    buffer->height = server->output_height > 0 ? server->output_height : 1920;
    buffer->stride = buffer->width * 4;
}

static struct shm_buffer *find_cached(struct wayland_server *server,
        struct wl_resource *resource) {
    struct shm_buffer *buffer;
    wl_list_for_each(buffer, &server->egl_buffers, egl_cache_link) {
        if (buffer->egl_resource == resource)
            return buffer;
    }
    return NULL;
}

static void egl_resource_destroyed(struct wl_listener *listener, void *data) {
    (void)data;
    struct shm_buffer *buffer =
            wl_container_of(listener, buffer, egl_resource_destroy_listener);
    buffer->egl_resource_destroy_listener_attached = false;
    trierarch_renderer_wait_idle(buffer->server ? buffer->server->renderer : NULL);
    if (buffer->egl_cache_linked) {
        wl_list_remove(&buffer->egl_cache_link);
        buffer->egl_cache_linked = false;
    }
    buffer->egl_resource = NULL;
    buffer->resource = NULL;
    /* Drop the cache reference.  Surfaces that still use the wrapper keep
     * their own references; the wrapper is freed when the last one goes. */
    trierarch_buffer_unref(buffer);
}

/* The EGL Wayland extension owns the actual wl_buffer implementation. We keep
 * one lightweight compositor-side wrapper per wl_buffer resource so repeated
 * attaches (every frame) reuse it instead of allocating a wrapper and a
 * destroy listener per commit.  The cache holds one reference. */
struct shm_buffer *trierarch_egl_buffer_from_resource(struct wl_resource *resource,
        struct wayland_server *server) {
    if (!resource || !server || !server->egl_buffer_supported)
        return NULL;
    struct shm_buffer *cached = find_cached(server, resource);
    if (cached) {
        apply_output_size(cached, server);
        return cached;
    }
    struct shm_buffer *buffer = calloc(1, sizeof(*buffer));
    if (!buffer) return NULL;
    buffer->references = 1;
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
    apply_output_size(buffer, server);
    buffer->format = WL_SHM_FORMAT_XRGB8888;
    wl_list_init(&buffer->egl_cache_link);
    wl_list_insert(&server->egl_buffers, &buffer->egl_cache_link);
    buffer->egl_cache_linked = true;
    return buffer;
}
