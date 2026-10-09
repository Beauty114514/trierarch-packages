#include "server_internal.h"

#include <android/log.h>
#include <stdlib.h>

#define TAG "trierarch-wayland"
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

static uint32_t resource_id(const struct wl_resource *resource) {
    return resource ? wl_resource_get_id((struct wl_resource *)resource) : 0;
}

/* KWin uses this global for nested helper and cursor surfaces. */
static void subsurface_destroy_request(struct wl_client *client,
        struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void subsurface_set_position(struct wl_client *client,
        struct wl_resource *resource, int32_t x, int32_t y) {
    (void)client;
    struct compositor_surface *surface = wl_resource_get_user_data(resource);
    if (!surface) return;
    surface->subsurface_x = x;
    surface->subsurface_y = y;
}

static void subsurface_place_above(struct wl_client *client,
        struct wl_resource *resource, struct wl_resource *sibling) {
    (void)client;
    struct compositor_surface *surface = wl_resource_get_user_data(resource);
    struct compositor_surface *sibling_surface =
            trierarch_surface_from_resource(sibling);
    if (!surface || !sibling_surface || !surface->parent ||
            (sibling_surface != surface->parent &&
             surface->parent != sibling_surface->parent)) {
        LOGW("subsurface place_above rejected: surface=%p id=%u parent=%p "
             "sibling=%p id=%u sibling-parent=%p",
             (void *)surface, resource_id(surface ? surface->wl_surface : NULL),
             (void *)(surface ? surface->parent : NULL),
             (void *)sibling_surface,
             resource_id(sibling),
             (void *)(sibling_surface ? sibling_surface->parent : NULL));
        wl_resource_post_error(resource, WL_SUBSURFACE_ERROR_BAD_SURFACE,
                "surface and sibling must share a parent");
        return;
    }
    wl_list_remove(&surface->subsurface_link);
    if (sibling_surface == surface->parent)
        wl_list_insert(surface->parent->children.prev, &surface->subsurface_link);
    else
        wl_list_insert(sibling_surface->subsurface_link.next, &surface->subsurface_link);
}

static void subsurface_place_below(struct wl_client *client,
        struct wl_resource *resource, struct wl_resource *sibling) {
    (void)client;
    struct compositor_surface *surface = wl_resource_get_user_data(resource);
    struct compositor_surface *sibling_surface =
            trierarch_surface_from_resource(sibling);
    if (!surface || !sibling_surface || !surface->parent ||
            (sibling_surface != surface->parent &&
             surface->parent != sibling_surface->parent)) {
        LOGW("subsurface place_below rejected: surface=%p id=%u parent=%p "
             "sibling=%p id=%u sibling-parent=%p",
             (void *)surface, resource_id(surface ? surface->wl_surface : NULL),
             (void *)(surface ? surface->parent : NULL),
             (void *)sibling_surface,
             resource_id(sibling),
             (void *)(sibling_surface ? sibling_surface->parent : NULL));
        wl_resource_post_error(resource, WL_SUBSURFACE_ERROR_BAD_SURFACE,
                "surface and sibling must share a parent");
        return;
    }
    wl_list_remove(&surface->subsurface_link);
    if (sibling_surface == surface->parent)
        wl_list_insert(&surface->parent->children, &surface->subsurface_link);
    else
        wl_list_insert(&sibling_surface->subsurface_link, &surface->subsurface_link);
}

static void subsurface_set_sync(struct wl_client *client,
        struct wl_resource *resource) {
    (void)client; (void)resource;
}

static void subsurface_set_desync(struct wl_client *client,
        struct wl_resource *resource) {
    (void)client; (void)resource;
}

static const struct wl_subsurface_interface subsurface_impl = {
    .destroy = subsurface_destroy_request,
    .set_position = subsurface_set_position,
    .place_above = subsurface_place_above,
    .place_below = subsurface_place_below,
    .set_sync = subsurface_set_sync,
    .set_desync = subsurface_set_desync,
};

static void subsurface_resource_destroy(struct wl_resource *resource) {
    struct compositor_surface *surface = wl_resource_get_user_data(resource);
    if (surface && surface->subsurface == resource) {
        surface->subsurface = NULL;
        if (surface->parent) {
            wl_list_remove(&surface->subsurface_link);
            wl_list_init(&surface->subsurface_link);
        }
        surface->parent = NULL;
    }
}

static void subcompositor_destroy_request(struct wl_client *client,
        struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void subcompositor_get_subsurface(struct wl_client *client,
        struct wl_resource *resource, uint32_t id,
        struct wl_resource *surface_resource,
        struct wl_resource *parent_resource) {
    struct compositor_surface *surface =
            trierarch_surface_from_resource(surface_resource);
    struct compositor_surface *parent =
            trierarch_surface_from_resource(parent_resource);
    if (!surface || !parent || surface == parent || surface->subsurface) {
        wl_resource_post_error(resource, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
                "invalid subsurface relationship");
        return;
    }
    struct wl_resource *subsurface = wl_resource_create(
            client, &wl_subsurface_interface,
            wl_resource_get_version(resource), id);
    if (!subsurface) {
        wl_client_post_no_memory(client);
        return;
    }
    surface->subsurface = subsurface;
    surface->parent = parent;
    wl_list_insert(&parent->children, &surface->subsurface_link);
    wl_resource_set_implementation(subsurface, &subsurface_impl, surface,
            subsurface_resource_destroy);
}

static const struct wl_subcompositor_interface subcompositor_impl = {
    .destroy = subcompositor_destroy_request,
    .get_subsurface = subcompositor_get_subsurface,
};

void trierarch_subcompositor_bind(struct wl_client *client, void *data,
        uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(
            client, &wl_subcompositor_interface, version < 1 ? version : 1, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &subcompositor_impl, data, NULL);
}
