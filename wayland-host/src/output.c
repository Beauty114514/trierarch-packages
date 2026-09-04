#include "server_internal.h"

#include <stdlib.h>

struct output_resource {
    struct wl_list link;
    struct wl_resource *resource;
};

static void output_release(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct wl_output_interface output_impl = {
    .release = output_release,
};

static void output_resource_destroy(struct wl_resource *resource) {
    struct output_resource *output = wl_resource_get_user_data(resource);
    if (!output) return;
    wl_list_remove(&output->link);
    free(output);
}

static void send_output_state(struct wayland_server *server, struct wl_resource *resource) {
    uint32_t version = (uint32_t)wl_resource_get_version(resource);
    int32_t width = server->output_width > 0 ? server->output_width : 1080;
    int32_t height = server->output_height > 0 ? server->output_height : 1920;

    wl_output_send_geometry(resource, 0, 0, width / 4, height / 4,
            WL_OUTPUT_SUBPIXEL_UNKNOWN, "Trierarch", "Wayland",
            WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(resource, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
            width, height, 60000);
    if (version >= 2) {
        wl_output_send_scale(resource, 1);
        wl_output_send_done(resource);
    }
    if (version >= 4) {
        wl_output_send_name(resource, "Trierarch-1");
        wl_output_send_description(resource, "Trierarch Wayland Output");
    }
}

void trierarch_output_bind(struct wl_client *client, void *data,
        uint32_t version, uint32_t id) {
    struct wayland_server *server = data;
    uint32_t bind_version = version < 4 ? version : 4;
    struct wl_resource *resource = wl_resource_create(
            client, &wl_output_interface, bind_version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }

    struct output_resource *output = calloc(1, sizeof(*output));
    if (!output) {
        wl_resource_destroy(resource);
        wl_client_post_no_memory(client);
        return;
    }
    output->resource = resource;
    wl_list_insert(&server->output_resources, &output->link);
    wl_resource_set_implementation(resource, &output_impl, output,
            output_resource_destroy);
    send_output_state(server, resource);
}

void trierarch_wayland_set_output_size(wayland_server_t *opaque, int width, int height) {
    struct wayland_server *server = (struct wayland_server *)opaque;
    if (!server) return;
    if (width <= 0 || height <= 0 ||
            (server->output_width == width && server->output_height == height)) return;
    server->output_width = width > 0 ? width : server->output_width;
    server->output_height = height > 0 ? height : server->output_height;

    struct output_resource *output;
    wl_list_for_each(output, &server->output_resources, link)
        send_output_state(server, output->resource);
    trierarch_xdg_output_notify(server);

    struct compositor_surface *surface;
    wl_list_for_each(surface, &server->surfaces, link)
        trierarch_surface_send_configure(surface);
    trierarch_wayland_request_render(server);
}

void trierarch_output_enter_surface(struct wayland_server *server,
        struct wl_resource *surface_resource) {
    if (!server || !surface_resource) return;
    struct wl_client *client = wl_resource_get_client(surface_resource);
    struct output_resource *output;
    wl_list_for_each(output, &server->output_resources, link) {
        if (output->resource && wl_resource_get_client(output->resource) == client)
            wl_surface_send_enter(surface_resource, output->resource);
    }
}
