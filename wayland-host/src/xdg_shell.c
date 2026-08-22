#include "server_internal.h"
#include "xdg-shell-server-protocol.h"

#include <stdlib.h>

static void popup_destroy(struct wl_client *, struct wl_resource *);
static void popup_grab(struct wl_client *, struct wl_resource *, struct wl_resource *, uint32_t);
static void popup_reposition(struct wl_client *, struct wl_resource *, struct wl_resource *, uint32_t);
static void positioner_destroy(struct wl_client *, struct wl_resource *);
static void positioner_set_size(struct wl_client *, struct wl_resource *, int32_t, int32_t);
static void positioner_set_anchor_rect(struct wl_client *, struct wl_resource *, int32_t, int32_t, int32_t, int32_t);
static void positioner_set_anchor(struct wl_client *, struct wl_resource *, uint32_t);
static void positioner_set_gravity(struct wl_client *, struct wl_resource *, uint32_t);
static void positioner_set_constraint(struct wl_client *, struct wl_resource *, uint32_t);
static void positioner_set_offset(struct wl_client *, struct wl_resource *, int32_t, int32_t);
static void positioner_set_reactive(struct wl_client *, struct wl_resource *);
static void positioner_set_parent_size(struct wl_client *, struct wl_resource *, int32_t, int32_t);
static void positioner_set_parent_configure(struct wl_client *, struct wl_resource *, uint32_t);

static void destroy_resource(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void xdg_toplevel_set_parent(struct wl_client *c, struct wl_resource *r, struct wl_resource *p) { (void)c;(void)r;(void)p; }
static void xdg_toplevel_set_title(struct wl_client *c, struct wl_resource *r, const char *t) { (void)c;(void)r;(void)t; }
static void xdg_toplevel_set_app_id(struct wl_client *c, struct wl_resource *r, const char *a) { (void)c;(void)r;(void)a; }
static void xdg_toplevel_show_window_menu(struct wl_client *c, struct wl_resource *r, struct wl_resource *s, uint32_t serial, int32_t x, int32_t y) { (void)c;(void)r;(void)s;(void)serial;(void)x;(void)y; }
static void xdg_toplevel_move(struct wl_client *c, struct wl_resource *r, struct wl_resource *s, uint32_t serial) { (void)c;(void)r;(void)s;(void)serial; }
static void xdg_toplevel_resize(struct wl_client *c, struct wl_resource *r, struct wl_resource *s, uint32_t serial, uint32_t edges) { (void)c;(void)r;(void)s;(void)serial;(void)edges; }
static void xdg_toplevel_set_max_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) { (void)c;(void)r;(void)w;(void)h; }
static void xdg_toplevel_set_min_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) { (void)c;(void)r;(void)w;(void)h; }
static void xdg_toplevel_set_maximized(struct wl_client *client,
        struct wl_resource *resource) {
    (void)client;
    struct compositor_surface *surface = wl_resource_get_user_data(resource);
    if (surface) trierarch_surface_send_configure(surface);
}

static void xdg_toplevel_unset_maximized(struct wl_client *client,
        struct wl_resource *resource) {
    (void)client;
    struct compositor_surface *surface = wl_resource_get_user_data(resource);
    if (surface) trierarch_surface_send_configure(surface);
}

static void xdg_toplevel_set_fullscreen(struct wl_client *client,
        struct wl_resource *resource, struct wl_resource *output) {
    (void)client;
    (void)output;
    struct compositor_surface *surface = wl_resource_get_user_data(resource);
    if (surface) trierarch_surface_send_configure(surface);
}

static void xdg_toplevel_unset_fullscreen(struct wl_client *client,
        struct wl_resource *resource) {
    (void)client;
    struct compositor_surface *surface = wl_resource_get_user_data(resource);
    if (surface) trierarch_surface_send_configure(surface);
}
static void xdg_toplevel_set_minimized(struct wl_client *c, struct wl_resource *r) { (void)c;(void)r; }

static void xdg_toplevel_destroy(struct wl_client *client, struct wl_resource *resource) {
    destroy_resource(client, resource);
}

static const struct xdg_toplevel_interface xdg_toplevel_impl = {
    .destroy = xdg_toplevel_destroy,
    .set_parent = xdg_toplevel_set_parent,
    .set_title = xdg_toplevel_set_title,
    .set_app_id = xdg_toplevel_set_app_id,
    .show_window_menu = xdg_toplevel_show_window_menu,
    .move = xdg_toplevel_move,
    .resize = xdg_toplevel_resize,
    .set_max_size = xdg_toplevel_set_max_size,
    .set_min_size = xdg_toplevel_set_min_size,
    .set_maximized = xdg_toplevel_set_maximized,
    .unset_maximized = xdg_toplevel_unset_maximized,
    .set_fullscreen = xdg_toplevel_set_fullscreen,
    .unset_fullscreen = xdg_toplevel_unset_fullscreen,
    .set_minimized = xdg_toplevel_set_minimized,
};

static void xdg_toplevel_resource_destroy(struct wl_resource *resource) {
    struct compositor_surface *surface = wl_resource_get_user_data(resource);
    if (surface) surface->xdg_toplevel = NULL;
}

static void xdg_surface_destroy(struct wl_client *client, struct wl_resource *resource) {
    destroy_resource(client, resource);
}

static void xdg_surface_get_toplevel(struct wl_client *client,
        struct wl_resource *resource, uint32_t id) {
    struct compositor_surface *surface = wl_resource_get_user_data(resource);
    if (!surface || surface->xdg_toplevel) {
        wl_resource_post_error(resource, XDG_WM_BASE_ERROR_ROLE,
                "surface already has a role");
        return;
    }
    struct wl_resource *toplevel = wl_resource_create(client, &xdg_toplevel_interface,
            wl_resource_get_version(resource), id);
    if (!toplevel) {
        wl_client_post_no_memory(client);
        return;
    }
    surface->xdg_toplevel = toplevel;
    wl_resource_set_implementation(toplevel, &xdg_toplevel_impl, surface,
            xdg_toplevel_resource_destroy);
    // Nested desktop clients need their initial configure before they commit
    // the first real output buffer. Advertise both states so KWin allocates
    // the fullscreen output instead of retaining its 1x1 bootstrap surface.
    trierarch_surface_send_configure(surface);
}

static void xdg_surface_get_popup(struct wl_client *client, struct wl_resource *resource,
        uint32_t id, struct wl_resource *parent, struct wl_resource *positioner) {
    (void)parent;
    (void)positioner;
    struct wl_resource *popup = wl_resource_create(client, &xdg_popup_interface,
            wl_resource_get_version(resource), id);
    if (!popup) {
        wl_client_post_no_memory(client);
        return;
    }
    /* Popups are used by Plasma during startup.  They must have a real
     * request table even though this fullscreen host does not position them. */
    static const struct xdg_popup_interface popup_impl = {
        .destroy = popup_destroy, .grab = popup_grab, .reposition = popup_reposition,
    };
    wl_resource_set_implementation(popup, &popup_impl, NULL, NULL);
    xdg_popup_send_configure(popup, 0, 0, 1, 1);
    xdg_surface_send_configure(resource, wl_display_next_serial(wl_client_get_display(client)));
}

static void xdg_surface_set_window_geometry(struct wl_client *client,
        struct wl_resource *resource, int32_t x, int32_t y, int32_t width, int32_t height) {
    (void)client; (void)resource; (void)x; (void)y; (void)width; (void)height;
}

static void xdg_surface_ack_configure(struct wl_client *client,
        struct wl_resource *resource, uint32_t serial) {
    (void)client; (void)resource; (void)serial;
}

static void xdg_surface_resource_destroy(struct wl_resource *resource) {
    struct compositor_surface *surface = wl_resource_get_user_data(resource);
    if (surface) surface->xdg_surface = NULL;
}

static const struct xdg_surface_interface xdg_surface_impl = {
    .destroy = xdg_surface_destroy,
    .get_toplevel = xdg_surface_get_toplevel,
    .get_popup = xdg_surface_get_popup,
    .set_window_geometry = xdg_surface_set_window_geometry,
    .ack_configure = xdg_surface_ack_configure,
};

static void wm_base_create_positioner(struct wl_client *client,
        struct wl_resource *resource, uint32_t id) {
    struct wl_resource *positioner = wl_resource_create(client, &xdg_positioner_interface,
            wl_resource_get_version(resource), id);
    if (!positioner) wl_client_post_no_memory(client);
    else {
        static const struct xdg_positioner_interface positioner_impl = {
            .destroy = positioner_destroy, .set_size = positioner_set_size,
            .set_anchor_rect = positioner_set_anchor_rect, .set_anchor = positioner_set_anchor,
            .set_gravity = positioner_set_gravity, .set_constraint_adjustment = positioner_set_constraint,
            .set_offset = positioner_set_offset, .set_reactive = positioner_set_reactive,
            .set_parent_size = positioner_set_parent_size,
            .set_parent_configure = positioner_set_parent_configure,
        };
        wl_resource_set_implementation(positioner, &positioner_impl, NULL, NULL);
    }
}

static void popup_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client; wl_resource_destroy(resource);
}
static void popup_grab(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *seat, uint32_t serial) {
    (void)client; (void)resource; (void)seat; (void)serial;
}
static void popup_reposition(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *positioner, uint32_t token) {
    (void)client; (void)positioner; xdg_popup_send_repositioned(resource, token);
}
static void positioner_destroy(struct wl_client *client, struct wl_resource *resource) { (void)client; wl_resource_destroy(resource); }
static void positioner_set_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) { (void)c;(void)r;(void)w;(void)h; }
static void positioner_set_anchor_rect(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) { (void)c;(void)r;(void)x;(void)y;(void)w;(void)h; }
static void positioner_set_anchor(struct wl_client *c, struct wl_resource *r, uint32_t value) { (void)c;(void)r;(void)value; }
static void positioner_set_gravity(struct wl_client *c, struct wl_resource *r, uint32_t value) { (void)c;(void)r;(void)value; }
static void positioner_set_constraint(struct wl_client *c, struct wl_resource *r, uint32_t value) { (void)c;(void)r;(void)value; }
static void positioner_set_offset(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y) { (void)c;(void)r;(void)x;(void)y; }
static void positioner_set_reactive(struct wl_client *c, struct wl_resource *r) { (void)c;(void)r; }
static void positioner_set_parent_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) { (void)c;(void)r;(void)w;(void)h; }
static void positioner_set_parent_configure(struct wl_client *c, struct wl_resource *r, uint32_t serial) { (void)c;(void)r;(void)serial; }

static void wm_base_get_xdg_surface(struct wl_client *client, struct wl_resource *resource,
        uint32_t id, struct wl_resource *surface_resource) {
    struct compositor_surface *surface = wl_resource_get_user_data(surface_resource);
    if (!surface || surface->server != wl_resource_get_user_data(resource) || surface->xdg_surface) {
        wl_resource_post_error(resource, XDG_WM_BASE_ERROR_INVALID_SURFACE_STATE,
                "invalid wl_surface");
        return;
    }
    struct wl_resource *xdg_surface = wl_resource_create(client, &xdg_surface_interface,
            wl_resource_get_version(resource), id);
    if (!xdg_surface) {
        wl_client_post_no_memory(client);
        return;
    }
    surface->xdg_surface = xdg_surface;
    wl_resource_set_implementation(xdg_surface, &xdg_surface_impl, surface,
            xdg_surface_resource_destroy);
}

static void wm_base_pong(struct wl_client *client, struct wl_resource *resource, uint32_t serial) {
    (void)client; (void)resource; (void)serial;
}

static const struct xdg_wm_base_interface wm_base_impl = {
    .destroy = destroy_resource,
    .create_positioner = wm_base_create_positioner,
    .get_xdg_surface = wm_base_get_xdg_surface,
    .pong = wm_base_pong,
};

void trierarch_surface_send_configure(struct compositor_surface *surface) {
    if (!surface || !surface->xdg_surface || !surface->xdg_toplevel) return;
    struct wl_array states;
    wl_array_init(&states);
    uint32_t *fullscreen = wl_array_add(&states, sizeof(*fullscreen));
    if (fullscreen) *fullscreen = XDG_TOPLEVEL_STATE_FULLSCREEN;
    uint32_t *maximized = wl_array_add(&states, sizeof(*maximized));
    if (maximized) *maximized = XDG_TOPLEVEL_STATE_MAXIMIZED;
    xdg_toplevel_send_configure(surface->xdg_toplevel,
            surface->server->output_width, surface->server->output_height, &states);
    wl_array_release(&states);
    xdg_surface_send_configure(surface->xdg_surface,
            wl_display_next_serial(surface->server->display));
    surface->configured = true;
}

void trierarch_xdg_shell_bind(struct wl_client *client, void *data,
        uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client, &xdg_wm_base_interface,
            version > 6 ? 6 : version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &wm_base_impl, data, NULL);
}
