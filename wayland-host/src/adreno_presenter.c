#include "server_internal.h"

#include <stdlib.h>
#include <unistd.h>

/* Zero capabilities keep this negotiation endpoint harmless until the host
 * allocation and fence path is enabled in the next step. */
#define TRIERARCH_ADRENO_CAPABILITIES 0u

struct adreno_buffer_request { struct wl_resource *resource; };

static void buffer_destroy(struct wl_resource *resource) {
    free(wl_resource_get_user_data(resource));
}

static void buffer_destroy_request(struct wl_client *client,
        struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void buffer_submit(struct wl_client *client, struct wl_resource *resource,
        int32_t acquire_fence) {
    (void)client;
    if (acquire_fence >= 0) close(acquire_fence);
    trierarch_adreno_buffer_v1_send_failed(resource,
            TRIERARCH_ADRENO_BUFFER_V1_ERROR_UNSUPPORTED);
}

static const struct trierarch_adreno_buffer_v1_interface buffer_impl = {
    .destroy = buffer_destroy_request,
    .submit = buffer_submit,
};

static void presenter_destroy(struct wl_client *client,
        struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void presenter_create_buffer(struct wl_client *client,
        struct wl_resource *resource, uint32_t id, int32_t width, int32_t height,
        uint32_t format, uint32_t flags) {
    (void)resource; (void)width; (void)height; (void)format; (void)flags;
    struct adreno_buffer_request *request = calloc(1, sizeof(*request));
    if (!request) { wl_client_post_no_memory(client); return; }
    request->resource = wl_resource_create(client,
            &trierarch_adreno_buffer_v1_interface, 1, id);
    if (!request->resource) {
        free(request);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(request->resource, &buffer_impl, request,
            buffer_destroy);
    trierarch_adreno_buffer_v1_send_failed(request->resource,
            TRIERARCH_ADRENO_BUFFER_V1_ERROR_UNSUPPORTED);
}

static const struct trierarch_adreno_presenter_v1_interface presenter_impl = {
    .destroy = presenter_destroy,
    .create_buffer = presenter_create_buffer,
};

void trierarch_adreno_presenter_bind(struct wl_client *client, void *data,
        uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client,
            &trierarch_adreno_presenter_v1_interface, version < 1 ? version : 1, id);
    if (!resource) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(resource, &presenter_impl, data, NULL);
    trierarch_adreno_presenter_v1_send_capabilities(resource,
            TRIERARCH_ADRENO_CAPABILITIES);
}
