#include "server_internal.h"

#include <stdint.h>
#include <time.h>

static void destroy_resource(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void presentation_feedback(struct wl_client *client,
        struct wl_resource *resource, struct wl_resource *surface, uint32_t callback) {
    (void)resource;
    (void)surface;
    struct wl_resource *feedback = wl_resource_create(client,
            &wp_presentation_feedback_interface, 1, callback);
    if (!feedback) {
        wl_client_post_no_memory(client);
        return;
    }

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        wp_presentation_feedback_send_discarded(feedback);
    } else {
        uint64_t seconds = (uint64_t)now.tv_sec;
        wp_presentation_feedback_send_presented(feedback,
                (uint32_t)(seconds >> 32), (uint32_t)seconds,
                (uint32_t)now.tv_nsec, 16666666, 0, 0,
                WP_PRESENTATION_FEEDBACK_KIND_VSYNC);
    }
    wl_resource_destroy(feedback);
}

static const struct wp_presentation_interface presentation_impl = {
    .destroy = destroy_resource,
    .feedback = presentation_feedback,
};

void trierarch_presentation_bind(struct wl_client *client, void *data,
        uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client,
            &wp_presentation_interface, version < 1 ? version : 1, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &presentation_impl, data, NULL);
    wp_presentation_send_clock_id(resource, CLOCK_MONOTONIC);
}
