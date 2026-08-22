#include "server_internal.h"

static void viewport_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void viewport_set_source(struct wl_client *client, struct wl_resource *resource,
        wl_fixed_t x, wl_fixed_t y, wl_fixed_t width, wl_fixed_t height) {
    (void)client;
    struct compositor_surface *surface = wl_resource_get_user_data(resource);
    if (!surface) return;
    if (x == wl_fixed_from_int(-1) && y == wl_fixed_from_int(-1) &&
            width == wl_fixed_from_int(-1) && height == wl_fixed_from_int(-1)) {
        surface->viewport_source_set = false;
        return;
    }
    surface->viewport_source_set = true;
    surface->viewport_source_x = x;
    surface->viewport_source_y = y;
    surface->viewport_source_width = width;
    surface->viewport_source_height = height;
}

static void viewport_set_destination(struct wl_client *client, struct wl_resource *resource,
        int32_t width, int32_t height) {
    (void)client;
    struct compositor_surface *surface = wl_resource_get_user_data(resource);
    if (!surface) return;
    if (width == -1 && height == -1) {
        surface->viewport_destination_set = false;
        return;
    }
    surface->viewport_destination_set = true;
    surface->viewport_destination_width = width;
    surface->viewport_destination_height = height;
}

static const struct wp_viewport_interface viewport_impl = {
    .destroy = viewport_destroy,
    .set_source = viewport_set_source,
    .set_destination = viewport_set_destination,
};

static void viewport_resource_destroy(struct wl_resource *resource) {
    struct compositor_surface *surface = wl_resource_get_user_data(resource);
    if (surface) surface->viewport = NULL;
}

static void viewporter_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void viewporter_get_viewport(struct wl_client *client, struct wl_resource *resource,
        uint32_t id, struct wl_resource *surface_resource) {
    struct compositor_surface *surface = trierarch_surface_from_resource(surface_resource);
    if (!surface) {
        wl_resource_post_error(resource, WP_VIEWPORT_ERROR_NO_SURFACE,
                "the wl_surface has been destroyed");
        return;
    }
    if (surface->viewport) {
        wl_resource_post_error(resource, WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS,
                "the wl_surface already has a viewport");
        return;
    }
    struct wl_resource *viewport = wl_resource_create(
            client, &wp_viewport_interface, 1, id);
    if (!viewport) {
        wl_client_post_no_memory(client);
        return;
    }
    surface->viewport = viewport;
    wl_resource_set_implementation(viewport, &viewport_impl, surface,
            viewport_resource_destroy);
}

static const struct wp_viewporter_interface viewporter_impl = {
    .destroy = viewporter_destroy,
    .get_viewport = viewporter_get_viewport,
};

void trierarch_viewporter_bind(struct wl_client *client, void *data,
        uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(
            client, &wp_viewporter_interface, version < 1 ? version : 1, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &viewporter_impl, data, NULL);
}
