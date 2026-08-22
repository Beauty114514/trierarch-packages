#include "server_internal.h"

#include <stdlib.h>

struct xdg_output_resource {
    struct wl_list link;
    struct wl_resource *resource;
};

static void send_output_state(struct wayland_server *server,
        struct wl_resource *resource) {
    uint32_t version = (uint32_t)wl_resource_get_version(resource);
    int32_t width = server && server->output_width > 0 ? server->output_width : 1080;
    int32_t height = server && server->output_height > 0 ? server->output_height : 1920;
    zxdg_output_v1_send_logical_position(resource, 0, 0);
    zxdg_output_v1_send_logical_size(resource, width, height);
    if (version >= 2) {
        zxdg_output_v1_send_name(resource, "Trierarch-1");
        zxdg_output_v1_send_description(resource, "Trierarch Wayland Output");
    }
    if (version < 3) zxdg_output_v1_send_done(resource);
}

static void output_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct zxdg_output_v1_interface output_impl = {
    .destroy = output_destroy,
};

static void output_resource_destroy(struct wl_resource *resource) {
    struct xdg_output_resource *output = wl_resource_get_user_data(resource);
    if (!output) return;
    wl_list_remove(&output->link);
    free(output);
}

static void manager_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void manager_get_output(struct wl_client *client, struct wl_resource *resource,
        uint32_t id, struct wl_resource *output) {
    (void)output;
    struct wayland_server *server = wl_resource_get_user_data(resource);
    uint32_t version = wl_resource_get_version(resource);
    struct wl_resource *xdg_output = wl_resource_create(client,
            &zxdg_output_v1_interface, version < 3 ? version : 3, id);
    if (!xdg_output) {
        wl_client_post_no_memory(client);
        return;
    }
    struct xdg_output_resource *tracked = calloc(1, sizeof(*tracked));
    if (!tracked) {
        wl_resource_destroy(xdg_output);
        wl_client_post_no_memory(client);
        return;
    }
    tracked->resource = xdg_output;
    wl_list_insert(&server->xdg_output_resources, &tracked->link);
    wl_resource_set_implementation(xdg_output, &output_impl, tracked,
            output_resource_destroy);
    send_output_state(server, xdg_output);
}

void trierarch_xdg_output_notify(struct wayland_server *server) {
    if (!server) return;
    struct xdg_output_resource *output;
    wl_list_for_each(output, &server->xdg_output_resources, link)
        send_output_state(server, output->resource);
}

static const struct zxdg_output_manager_v1_interface manager_impl = {
    .destroy = manager_destroy,
    .get_xdg_output = manager_get_output,
};

void trierarch_xdg_output_bind(struct wl_client *client, void *data,
        uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client,
            &zxdg_output_manager_v1_interface, version < 3 ? version : 3, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, NULL);
}
