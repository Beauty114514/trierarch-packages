#include "server_internal.h"

#include <stdlib.h>

/* This is intentionally a small compositor-side implementation.  It covers
 * the state and geometry needed by desktop wallpaper/panel clients without
 * making the host a wlroots compositor. */

static void layer_shell_destroy(struct wl_client *client,
        struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void layer_surface_destroy_request(struct wl_client *client,
        struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static struct compositor_surface *layer_surface_from_resource(
        struct wl_resource *resource) {
    return resource ? wl_resource_get_user_data(resource) : NULL;
}

static void layer_surface_set_size(struct wl_client *client,
        struct wl_resource *resource, uint32_t width, uint32_t height) {
    (void)client;
    struct compositor_surface *surface = layer_surface_from_resource(resource);
    if (!surface) return;
    if (width > 16384 || height > 16384) {
        wl_resource_post_error(resource,
                ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SIZE,
                "layer surface size is too large");
        return;
    }
    surface->layer_width = width;
    surface->layer_height = height;
}

static void layer_surface_set_anchor(struct wl_client *client,
        struct wl_resource *resource, uint32_t anchor) {
    (void)client;
    struct compositor_surface *surface = layer_surface_from_resource(resource);
    if (!surface) return;
    if ((anchor & ~(ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                    ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                    ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                    ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)) != 0) {
        wl_resource_post_error(resource,
                ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_ANCHOR,
                "invalid layer surface anchor");
        return;
    }
    surface->layer_anchor = anchor;
}

static void layer_surface_set_exclusive_zone(struct wl_client *client,
        struct wl_resource *resource, int32_t zone) {
    (void)client;
    struct compositor_surface *surface = layer_surface_from_resource(resource);
    if (surface) surface->layer_exclusive_zone = zone;
}

static void layer_surface_set_margin(struct wl_client *client,
        struct wl_resource *resource, int32_t top, int32_t right,
        int32_t bottom, int32_t left) {
    (void)client;
    struct compositor_surface *surface = layer_surface_from_resource(resource);
    if (!surface) return;
    surface->layer_margin_top = top;
    surface->layer_margin_right = right;
    surface->layer_margin_bottom = bottom;
    surface->layer_margin_left = left;
}

static void layer_surface_set_keyboard_interactivity(struct wl_client *client,
        struct wl_resource *resource, uint32_t interactivity) {
    (void)client;
    if (interactivity > ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) {
        wl_resource_post_error(resource,
                ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_KEYBOARD_INTERACTIVITY,
                "invalid layer surface keyboard interactivity");
    }
}

static void layer_surface_get_popup(struct wl_client *client,
        struct wl_resource *resource, struct wl_resource *popup) {
    (void)client;
    (void)resource;
    (void)popup;
    /* Popups are still handled through the normal xdg-shell implementation. */
}

static void layer_surface_ack_configure(struct wl_client *client,
        struct wl_resource *resource, uint32_t serial) {
    (void)client;
    (void)resource;
    (void)serial;
}

static void layer_surface_set_layer(struct wl_client *client,
        struct wl_resource *resource, uint32_t layer) {
    (void)client;
    struct compositor_surface *surface = layer_surface_from_resource(resource);
    if (!surface) return;
    if (layer > ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY) {
        wl_resource_post_error(resource,
                ZWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER,
                "invalid layer");
        return;
    }
    surface->layer_kind = layer;
}

static const struct zwlr_layer_surface_v1_interface layer_surface_impl = {
    .set_size = layer_surface_set_size,
    .set_anchor = layer_surface_set_anchor,
    .set_exclusive_zone = layer_surface_set_exclusive_zone,
    .set_margin = layer_surface_set_margin,
    .set_keyboard_interactivity = layer_surface_set_keyboard_interactivity,
    .get_popup = layer_surface_get_popup,
    .ack_configure = layer_surface_ack_configure,
    .destroy = layer_surface_destroy_request,
    .set_layer = layer_surface_set_layer,
};

static void layer_surface_resource_destroy(struct wl_resource *resource) {
    struct compositor_surface *surface = layer_surface_from_resource(resource);
    if (!surface) return;
    surface->layer_surface = NULL;
    surface->layer_configured = false;
    surface->mapped = false;
    trierarch_wayland_request_render(surface->server);
}

static void layer_shell_get_layer_surface(struct wl_client *client,
        struct wl_resource *resource, uint32_t id, struct wl_resource *surface_resource,
        struct wl_resource *output_resource, uint32_t layer, const char *namespace) {
    (void)output_resource;
    (void)namespace;
    struct compositor_surface *surface = trierarch_surface_from_resource(surface_resource);
    if (!surface || surface->xdg_surface || surface->xdg_toplevel ||
            surface->subsurface || surface->layer_surface) {
        wl_resource_post_error(resource, ZWLR_LAYER_SHELL_V1_ERROR_ROLE,
                "surface already has a role");
        return;
    }
    if (layer > ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY) {
        wl_resource_post_error(resource, ZWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER,
                "invalid layer");
        return;
    }
    if (surface->current || surface->pending) {
        wl_resource_post_error(resource, ZWLR_LAYER_SHELL_V1_ERROR_ALREADY_CONSTRUCTED,
                "surface was committed before layer role");
        return;
    }
    struct wl_resource *layer_resource = wl_resource_create(client,
            &zwlr_layer_surface_v1_interface,
            wl_resource_get_version(resource) < 4 ? wl_resource_get_version(resource) : 4,
            id);
    if (!layer_resource) {
        wl_client_post_no_memory(client);
        return;
    }
    surface->layer_surface = layer_resource;
    surface->layer_kind = layer;
    surface->layer_anchor = 0;
    surface->layer_exclusive_zone = 0;
    wl_resource_set_implementation(layer_resource, &layer_surface_impl, surface,
            layer_surface_resource_destroy);
}

static const struct zwlr_layer_shell_v1_interface layer_shell_impl = {
    .get_layer_surface = layer_shell_get_layer_surface,
    .destroy = layer_shell_destroy,
};

void trierarch_layer_shell_bind(struct wl_client *client, void *data,
        uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client,
            &zwlr_layer_shell_v1_interface, version < 4 ? version : 4, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &layer_shell_impl, data, NULL);
}

static int32_t clamp_dimension(int32_t value, int32_t maximum) {
    if (value < 1) return 1;
    return value > maximum ? maximum : value;
}

static int32_t anchored_size(uint32_t requested, bool first, bool second,
        int32_t output, int32_t first_margin, int32_t second_margin) {
    if (requested) return clamp_dimension((int32_t)requested, output);
    if (first && second)
        return clamp_dimension(output - first_margin - second_margin, output);
    return clamp_dimension(output, output);
}

static int32_t anchored_position(uint32_t requested, bool first, bool second,
        int32_t output, int32_t size, int32_t first_margin, int32_t second_margin) {
    if (first && second) return first_margin;
    if (first) return first_margin;
    if (second) return output - size - second_margin;
    return (output - size) / 2 + (first_margin - second_margin) / 2;
}

void trierarch_layer_surface_commit(struct compositor_surface *surface) {
    if (!surface || !surface->layer_surface) return;
    const uint32_t anchor = surface->layer_anchor;
    const int32_t width = anchored_size(surface->layer_width,
            anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT,
            anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
            surface->server->output_width,
            surface->layer_margin_left, surface->layer_margin_right);
    const int32_t height = anchored_size(surface->layer_height,
            anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP,
            anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM,
            surface->server->output_height,
            surface->layer_margin_top, surface->layer_margin_bottom);
    surface->layer_x = anchored_position(surface->layer_width,
            anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT,
            anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
            surface->server->output_width, width,
            surface->layer_margin_left, surface->layer_margin_right);
    surface->layer_y = anchored_position(surface->layer_height,
            anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP,
            anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM,
            surface->server->output_height, height,
            surface->layer_margin_top, surface->layer_margin_bottom);
    if (!surface->layer_configured) {
        surface->layer_configured = true;
        zwlr_layer_surface_v1_send_configure(surface->layer_surface,
                wl_display_next_serial(wl_client_get_display(
                        wl_resource_get_client(surface->layer_surface))),
                (uint32_t)width, (uint32_t)height);
    }
}
