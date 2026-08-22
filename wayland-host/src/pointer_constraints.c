#include "server_internal.h"

#include <stdint.h>

static void destroy_resource(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void locked_set_cursor_position_hint(struct wl_client *client,
        struct wl_resource *resource, wl_fixed_t surface_x, wl_fixed_t surface_y) {
    (void)client;
    (void)resource;
    (void)surface_x;
    (void)surface_y;
}

static void locked_set_region(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *region) {
    (void)client;
    (void)resource;
    (void)region;
}

static const struct zwp_locked_pointer_v1_interface locked_impl = {
    .destroy = destroy_resource,
    .set_cursor_position_hint = locked_set_cursor_position_hint,
    .set_region = locked_set_region,
};

static void confined_set_region(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *region) {
    (void)client;
    (void)resource;
    (void)region;
}

static const struct zwp_confined_pointer_v1_interface confined_impl = {
    .destroy = destroy_resource,
    .set_region = confined_set_region,
};

static void create_locked_pointer(struct wl_client *client, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client,
            &zwp_locked_pointer_v1_interface, 1, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &locked_impl, NULL, NULL);
    zwp_locked_pointer_v1_send_locked(resource);
}

static void create_confined_pointer(struct wl_client *client, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client,
            &zwp_confined_pointer_v1_interface, 1, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &confined_impl, NULL, NULL);
    zwp_confined_pointer_v1_send_confined(resource);
}

static void constraints_lock_pointer(struct wl_client *client,
        struct wl_resource *resource, uint32_t id, struct wl_resource *surface,
        struct wl_resource *pointer, struct wl_resource *region, uint32_t lifetime) {
    (void)resource;
    (void)surface;
    (void)pointer;
    (void)region;
    (void)lifetime;
    create_locked_pointer(client, id);
}

static void constraints_confine_pointer(struct wl_client *client,
        struct wl_resource *resource, uint32_t id, struct wl_resource *surface,
        struct wl_resource *pointer, struct wl_resource *region, uint32_t lifetime) {
    (void)resource;
    (void)surface;
    (void)pointer;
    (void)region;
    (void)lifetime;
    create_confined_pointer(client, id);
}

static const struct zwp_pointer_constraints_v1_interface constraints_impl = {
    .destroy = destroy_resource,
    .lock_pointer = constraints_lock_pointer,
    .confine_pointer = constraints_confine_pointer,
};

void trierarch_pointer_constraints_bind(struct wl_client *client, void *data,
        uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client,
            &zwp_pointer_constraints_v1_interface, version < 1 ? version : 1, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &constraints_impl, data, NULL);
}
