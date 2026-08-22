#include "server_internal.h"

#include <stdlib.h>

static void single_pixel_destroy(struct wl_client *client,
        struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct wl_buffer_interface single_pixel_buffer_impl = {
    .destroy = single_pixel_destroy,
};

static void single_pixel_buffer_resource_destroy(struct wl_resource *resource) {
    struct shm_buffer *buffer = wl_resource_get_user_data(resource);
    if (!buffer) return;
    buffer->resource = NULL;
    free(buffer->data);
    free(buffer);
}

static struct wl_resource *single_pixel_buffer_create(struct wl_client *client,
        uint32_t id, uint32_t pixel) {
    struct shm_buffer *buffer = calloc(1, sizeof(*buffer));
    if (!buffer) return NULL;
    buffer->data = malloc(4);
    if (!buffer->data) {
        free(buffer);
        return NULL;
    }
    *(uint32_t *)buffer->data = pixel;
    buffer->size = 4;
    buffer->width = 1;
    buffer->height = 1;
    buffer->stride = 4;
    buffer->format = WL_SHM_FORMAT_ARGB8888;
    buffer->single_pixel = true;
    buffer->resource = wl_resource_create(client, &wl_buffer_interface, 1, id);
    if (!buffer->resource) {
        free(buffer->data);
        free(buffer);
        return NULL;
    }
    wl_resource_set_implementation(buffer->resource, &single_pixel_buffer_impl,
            buffer, single_pixel_buffer_resource_destroy);
    return buffer->resource;
}

static void manager_destroy(struct wl_client *client,
        struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void manager_create_u32_rgba_buffer(struct wl_client *client,
        struct wl_resource *resource, uint32_t id,
        uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    (void)resource;
    uint32_t pixel = ((a >> 24) << 24) | ((r >> 24) << 16) |
            ((g >> 24) << 8) | (b >> 24);
    if (!single_pixel_buffer_create(client, id, pixel))
        wl_client_post_no_memory(client);
}

static const struct wp_single_pixel_buffer_manager_v1_interface manager_impl = {
    .destroy = manager_destroy,
    .create_u32_rgba_buffer = manager_create_u32_rgba_buffer,
};

void trierarch_single_pixel_bind(struct wl_client *client, void *data,
        uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *resource = wl_resource_create(client,
            &wp_single_pixel_buffer_manager_v1_interface,
            version < 1 ? version : 1, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}
