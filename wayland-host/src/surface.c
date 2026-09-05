#include "server_internal.h"

#include <stdlib.h>
#include <android/log.h>

#define TRIERARCH_TAG "TrierarchWayland"

static void region_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void region_add(struct wl_client *client, struct wl_resource *resource,
        int32_t x, int32_t y, int32_t width, int32_t height) {
    (void)client; (void)resource; (void)x; (void)y; (void)width; (void)height;
}

static void region_subtract(struct wl_client *client, struct wl_resource *resource,
        int32_t x, int32_t y, int32_t width, int32_t height) {
    (void)client; (void)resource; (void)x; (void)y; (void)width; (void)height;
}

static const struct wl_region_interface region_impl = {
    .destroy = region_destroy,
    .add = region_add,
    .subtract = region_subtract,
};

static void compositor_create_region(struct wl_client *client,
        struct wl_resource *resource, uint32_t id) {
    struct wl_resource *region = wl_resource_create(
            client, &wl_region_interface, wl_resource_get_version(resource), id);
    if (!region) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(region, &region_impl, NULL, NULL);
}

static void frame_callback_destroy(struct wl_resource *resource) {
    struct surface_frame_callback *callback = wl_resource_get_user_data(resource);
    if (!callback) return;
    wl_list_remove(&callback->link);
    free(callback);
}

static void surface_frame(struct wl_client *client, struct wl_resource *resource,
        uint32_t callback_id) {
    struct compositor_surface *surface = wl_resource_get_user_data(resource);
    if (!surface) return;
    struct surface_frame_callback *callback = calloc(1, sizeof(*callback));
    if (!callback) {
        wl_client_post_no_memory(client);
        return;
    }
    callback->resource = wl_resource_create(client, &wl_callback_interface, 1, callback_id);
    if (!callback->resource) {
        free(callback);
        wl_client_post_no_memory(client);
        return;
    }
    wl_list_insert(surface->pending_frame_callbacks.prev, &callback->link);
    surface->perf_frame_callbacks_requested++;
    surface->server->perf_frame_callbacks_requested++;
    wl_resource_set_implementation(callback->resource, NULL, callback,
            frame_callback_destroy);
}

static void surface_attach(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *buffer_resource, int32_t x, int32_t y) {
    (void)client;
    struct compositor_surface *surface = wl_resource_get_user_data(resource);
    if (!surface || x != 0 || y != 0) {
        wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_OFFSET,
                "non-zero buffer offsets are not supported");
        return;
    }
    surface->pending = buffer_resource
            ? trierarch_shm_buffer_from_resource(buffer_resource) : NULL;
    if (buffer_resource && !surface->pending)
        surface->pending = trierarch_dmabuf_buffer_from_resource(buffer_resource);
    if (buffer_resource && !surface->pending)
        surface->pending = trierarch_android_buffer_from_resource(buffer_resource);
    if (buffer_resource && !surface->pending)
        surface->pending = trierarch_egl_buffer_from_resource(buffer_resource, surface->server);
    if (buffer_resource && !surface->pending) {
        wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_SIZE,
                "unsupported wl_buffer type");
    } else if (surface->pending) {
        static unsigned int attach_logs;
        if (attach_logs++ < 64) {
            __android_log_print(ANDROID_LOG_INFO, TRIERARCH_TAG,
                    "attach buffer type=%s size=%dx%d format=0x%x",
                    surface->pending->dmabuf ? "dmabuf" :
                    surface->pending->android_buffer ? "android-wlegl" :
                    surface->pending->egl_buffer ? "egl-wl" : "shm",
                    surface->pending->width, surface->pending->height,
                    surface->pending->format);
        }
    }
}

static void surface_damage(struct wl_client *client, struct wl_resource *resource,
        int32_t x, int32_t y, int32_t width, int32_t height) {
    (void)client; (void)x; (void)y; (void)width; (void)height;
    struct compositor_surface *surface = wl_resource_get_user_data(resource);
    if (surface) {
        surface->damaged = true;
        surface->perf_damage++;
        surface->server->perf_surface_damage++;
        surface->server->perf_surface_damage_generation++;
        trierarch_wayland_request_render(surface->server);
    }
}

static void surface_damage_buffer(struct wl_client *client, struct wl_resource *resource,
        int32_t x, int32_t y, int32_t width, int32_t height) {
    surface_damage(client, resource, x, y, width, height);
}

static void surface_commit(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    struct compositor_surface *surface = wl_resource_get_user_data(resource);
    if (surface) trierarch_surface_commit(surface);
}

static void surface_destroy_request(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void surface_set_opaque_region(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *region) {
    (void)client; (void)resource;
    (void)region;
}

static void surface_set_input_region(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *region) {
    (void)client; (void)resource;
    (void)region;
}

static void surface_set_buffer_transform(struct wl_client *client, struct wl_resource *resource,
        int32_t transform) {
    (void)client; (void)resource; (void)transform;
}

static void surface_set_buffer_scale(struct wl_client *client, struct wl_resource *resource,
        int32_t scale) {
    (void)client;
    struct compositor_surface *surface = wl_resource_get_user_data(resource);
    if (surface && scale > 0) surface->buffer_scale = scale;
}

static void surface_offset(struct wl_client *client, struct wl_resource *resource,
        int32_t x, int32_t y) {
    (void)client; (void)resource; (void)x; (void)y;
}

static const struct wl_surface_interface surface_impl = {
    .destroy = surface_destroy_request,
    .attach = surface_attach,
    .damage = surface_damage,
    .frame = surface_frame,
    .set_opaque_region = surface_set_opaque_region,
    .set_input_region = surface_set_input_region,
    .commit = surface_commit,
    .set_buffer_transform = surface_set_buffer_transform,
    .set_buffer_scale = surface_set_buffer_scale,
    .damage_buffer = surface_damage_buffer,
    .offset = surface_offset,
};

static void surface_resource_destroy(struct wl_resource *resource) {
    struct compositor_surface *surface = wl_resource_get_user_data(resource);
    if (!surface) return;
    if (surface->parent) {
        wl_list_remove(&surface->subsurface_link);
        wl_list_init(&surface->subsurface_link);
        surface->parent = NULL;
    }
    /* Do not leave a live child pointing at a freed parent surface.  Its
     * wl_subsurface object remains owned by the client and will be destroyed
     * through the normal protocol path. */
    struct compositor_surface *child, *next_child;
    wl_list_for_each_safe(child, next_child, &surface->children, subsurface_link) {
        wl_list_remove(&child->subsurface_link);
        wl_list_init(&child->subsurface_link);
        child->parent = NULL;
    }
    if (surface->server && surface->server->pointer_focus == surface)
        surface->server->pointer_focus = NULL;
    if (surface->server && surface->server->cursor_surface == surface) {
        surface->server->cursor_surface = NULL;
        surface->server->cursor_hotspot_x = 0;
        surface->server->cursor_hotspot_y = 0;
    }
    trierarch_wayland_request_render(surface->server);
    if (surface->current) {
        struct shm_buffer *current = surface->current;
        trierarch_shm_buffer_release(current);
        if (current->egl_buffer) free(current);
    }
    if (surface->pending && surface->pending->egl_buffer)
        free(surface->pending);
    wl_list_remove(&surface->link);
    free(surface);
}

static void compositor_create_surface(struct wl_client *client,
        struct wl_resource *resource, uint32_t id) {
    struct wayland_server *server = wl_resource_get_user_data(resource);
    struct compositor_surface *surface = calloc(1, sizeof(*surface));
    if (!surface) {
        wl_client_post_no_memory(client);
        return;
    }
    surface->server = server;
    surface->buffer_scale = 1;
    wl_list_init(&surface->children);
    wl_list_init(&surface->subsurface_link);
    wl_list_init(&surface->pending_frame_callbacks);
    wl_list_init(&surface->committed_frame_callbacks);
    wl_list_init(&surface->presented_frame_callbacks);
    surface->wl_surface = wl_resource_create(
            client, &wl_surface_interface, wl_resource_get_version(resource), id);
    if (!surface->wl_surface) {
        free(surface);
        wl_client_post_no_memory(client);
        return;
    }
    wl_list_insert(&server->surfaces, &surface->link);
    wl_resource_set_implementation(surface->wl_surface, &surface_impl, surface,
            surface_resource_destroy);
}

static void compositor_release(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct wl_compositor_interface compositor_impl = {
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
    .release = compositor_release,
};

void trierarch_surface_bind(struct wl_client *client, void *data,
        uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(
            client, &wl_compositor_interface, version < 4 ? version : 4, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &compositor_impl, data, NULL);
}

struct compositor_surface *trierarch_surface_from_resource(struct wl_resource *resource) {
    return resource ? wl_resource_get_user_data(resource) : NULL;
}

void trierarch_surface_commit(struct compositor_surface *surface) {
    if (!surface) return;
    surface->perf_commits++;
    surface->server->perf_surface_commits++;
    surface->server->perf_surface_commit_generation++;
    if (!wl_list_empty(&surface->pending_frame_callbacks)) {
        uint64_t callbacks = (uint64_t)wl_list_length(&surface->pending_frame_callbacks);
        surface->perf_frame_callbacks_committed += callbacks;
        surface->server->perf_frame_callbacks_committed += callbacks;
        wl_list_insert_list(surface->committed_frame_callbacks.prev,
                &surface->pending_frame_callbacks);
        wl_list_init(&surface->pending_frame_callbacks);
    }
    if (surface->pending) {
        if (surface->pending != surface->current)
            surface->perf_buffer_replacements++;
        if (surface->current) {
            struct shm_buffer *current = surface->current;
            trierarch_shm_buffer_release(current);
            if (current->egl_buffer) free(current);
        }
        surface->current = surface->pending;
        surface->current->busy = true;
        surface->pending = NULL;
        surface->width = surface->current->width;
        surface->height = surface->current->height;
        surface->mapped = true;
        surface->damaged = true;
        /* Nested compositors such as KWin wait for the surface to enter an
         * output before committing their real desktop buffer. */
        trierarch_output_enter_surface(surface->server, surface->wl_surface);
    }
    if (surface->xdg_surface && !surface->configured)
        trierarch_surface_send_configure(surface);
    trierarch_wayland_request_render(surface->server);
}

static bool surface_participates_in_output(const struct wayland_server *server,
        const struct compositor_surface *surface) {
    if (!surface->mapped || !surface->current) return false;
    if (surface == server->cursor_surface) return server->cursor_visible;
    for (const struct compositor_surface *parent = surface->parent; parent;
            parent = parent->parent) {
        if (!parent->mapped || !parent->current || parent == server->cursor_surface)
            return false;
    }
    return true;
}

void trierarch_surface_latch_frame_callbacks(struct wayland_server *server) {
    struct compositor_surface *surface;
    wl_list_for_each(surface, &server->surfaces, link) {
        if (!surface_participates_in_output(server, surface) ||
                wl_list_empty(&surface->committed_frame_callbacks))
            continue;
        uint64_t callbacks = (uint64_t)wl_list_length(&surface->committed_frame_callbacks);
        surface->perf_frame_callbacks_captured += callbacks;
        server->perf_frame_callbacks_captured += callbacks;
        wl_list_insert_list(surface->presented_frame_callbacks.prev,
                &surface->committed_frame_callbacks);
        wl_list_init(&surface->committed_frame_callbacks);
    }
}

void trierarch_surface_requeue_frame_callbacks(struct wayland_server *server) {
    struct compositor_surface *surface;
    wl_list_for_each(surface, &server->surfaces, link) {
        if (wl_list_empty(&surface->presented_frame_callbacks)) continue;
        wl_list_insert_list(surface->committed_frame_callbacks.prev,
                &surface->presented_frame_callbacks);
        wl_list_init(&surface->presented_frame_callbacks);
    }
}

void trierarch_surface_send_frame_callbacks(struct wayland_server *server, uint32_t time_ms) {
    struct compositor_surface *surface;
    wl_list_for_each(surface, &server->surfaces, link) {
        struct surface_frame_callback *callback, *tmp;
        wl_list_for_each_safe(callback, tmp, &surface->presented_frame_callbacks, link) {
            wl_callback_send_done(callback->resource, time_ms);
            surface->perf_frame_callbacks_completed++;
            server->perf_frame_callbacks_completed++;
            wl_resource_destroy(callback->resource);
        }
    }
}
