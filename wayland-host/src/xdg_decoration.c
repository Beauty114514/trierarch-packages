#include "server_internal.h"

static void decoration_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void decoration_set_mode(struct wl_client *client, struct wl_resource *resource,
        uint32_t mode) {
    (void)client;
    (void)mode;
    zxdg_toplevel_decoration_v1_send_configure(resource,
            ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void decoration_unset_mode(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    zxdg_toplevel_decoration_v1_send_configure(resource,
            ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static const struct zxdg_toplevel_decoration_v1_interface decoration_impl = {
    .destroy = decoration_destroy,
    .set_mode = decoration_set_mode,
    .unset_mode = decoration_unset_mode,
};

static void manager_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void manager_get_decoration(struct wl_client *client, struct wl_resource *resource,
        uint32_t id, struct wl_resource *toplevel) {
    (void)resource;
    (void)toplevel;
    struct wl_resource *decoration = wl_resource_create(client,
            &zxdg_toplevel_decoration_v1_interface, 1, id);
    if (!decoration) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(decoration, &decoration_impl, NULL, NULL);
    zxdg_toplevel_decoration_v1_send_configure(decoration,
            ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static const struct zxdg_decoration_manager_v1_interface manager_impl = {
    .destroy = manager_destroy,
    .get_toplevel_decoration = manager_get_decoration,
};

void trierarch_xdg_decoration_bind(struct wl_client *client, void *data,
        uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client,
            &zxdg_decoration_manager_v1_interface, version < 1 ? version : 1, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, NULL);
}
