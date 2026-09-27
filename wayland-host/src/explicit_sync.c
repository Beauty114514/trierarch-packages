#include "explicit_sync.h"

#include "server_internal.h"

#include <stdlib.h>
#include <unistd.h>

struct trierarch_explicit_release {
    unsigned int references;
    struct wl_resource *resource;
};

static void close_fence(int *fd) {
    if (*fd >= 0)
        close(*fd);
    *fd = -1;
}

static void release_unref(struct trierarch_explicit_release *release) {
    if (release && --release->references == 0)
        free(release);
}

static void release_resource_destroy(struct wl_resource *resource) {
    struct trierarch_explicit_release *release = wl_resource_get_user_data(resource);
    if (!release)
        return;
    release->resource = NULL;
    release_unref(release);
}

void trierarch_explicit_release_complete(struct trierarch_explicit_release *release) {
    if (!release)
        return;
    if (release->resource) {
        zwp_linux_buffer_release_v1_send_immediate_release(release->resource);
        wl_resource_destroy(release->resource);
    }
    release_unref(release);
}

void trierarch_explicit_sync_state_init(struct trierarch_explicit_sync_state *state) {
    if (!state)
        return;
    state->pending_acquire_fence_fd = -1;
    state->current_acquire_fence_fd = -1;
    state->pending_release = NULL;
    state->current_release = NULL;
    state->resource = NULL;
}

void trierarch_explicit_sync_state_commit(struct trierarch_explicit_sync_state *state,
        bool attached) {
    if (!state || !attached)
        return;
    close_fence(&state->current_acquire_fence_fd);
    trierarch_explicit_release_complete(state->current_release);
    state->current_acquire_fence_fd = state->pending_acquire_fence_fd;
    state->pending_acquire_fence_fd = -1;
    state->current_release = state->pending_release;
    state->pending_release = NULL;
}

bool trierarch_explicit_sync_state_has_pending(
        const struct trierarch_explicit_sync_state *state) {
    return state && (state->pending_acquire_fence_fd >= 0 || state->pending_release);
}

void trierarch_explicit_sync_state_discard_pending(
        struct trierarch_explicit_sync_state *state) {
    if (!state)
        return;
    close_fence(&state->pending_acquire_fence_fd);
    trierarch_explicit_release_complete(state->pending_release);
    state->pending_release = NULL;
}

int trierarch_explicit_sync_state_take_acquire_fence(
        struct trierarch_explicit_sync_state *state) {
    if (!state)
        return -1;
    int fd = state->current_acquire_fence_fd;
    state->current_acquire_fence_fd = -1;
    return fd;
}

struct trierarch_explicit_release *trierarch_explicit_sync_state_take_release(
        struct trierarch_explicit_sync_state *state) {
    if (!state)
        return NULL;
    struct trierarch_explicit_release *release = state->current_release;
    state->current_release = NULL;
    return release;
}

void trierarch_explicit_sync_state_destroy(struct trierarch_explicit_sync_state *state) {
    if (!state)
        return;
    trierarch_explicit_sync_state_discard_pending(state);
    close_fence(&state->current_acquire_fence_fd);
    trierarch_explicit_release_complete(state->current_release);
    state->current_release = NULL;
    if (state->resource) {
        wl_resource_set_user_data(state->resource, NULL);
        state->resource = NULL;
    }
}

static void sync_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void sync_resource_destroy(struct wl_resource *resource) {
    struct compositor_surface *surface = wl_resource_get_user_data(resource);
    if (!surface)
        return;
    if (surface->explicit_sync.resource == resource)
        surface->explicit_sync.resource = NULL;
    close_fence(&surface->explicit_sync.pending_acquire_fence_fd);
}

static void sync_set_acquire_fence(struct wl_client *client, struct wl_resource *resource,
        int32_t fd) {
    (void)client;
    struct compositor_surface *surface = wl_resource_get_user_data(resource);
    if (!surface) {
        if (fd >= 0)
            close(fd);
        wl_resource_post_error(resource,
                ZWP_LINUX_SURFACE_SYNCHRONIZATION_V1_ERROR_NO_SURFACE,
                "the wl_surface was destroyed");
        return;
    }
    if (fd < 0) {
        wl_resource_post_error(resource,
                ZWP_LINUX_SURFACE_SYNCHRONIZATION_V1_ERROR_INVALID_FENCE,
                "invalid acquire fence");
        return;
    }
    if (surface->explicit_sync.pending_acquire_fence_fd >= 0) {
        close(fd);
        wl_resource_post_error(resource,
                ZWP_LINUX_SURFACE_SYNCHRONIZATION_V1_ERROR_DUPLICATE_FENCE,
                "acquire fence already set for this commit");
        return;
    }
    surface->explicit_sync.pending_acquire_fence_fd = fd;
}

static void sync_get_release(struct wl_client *client, struct wl_resource *resource,
        uint32_t id) {
    struct compositor_surface *surface = wl_resource_get_user_data(resource);
    if (!surface) {
        wl_resource_post_error(resource,
                ZWP_LINUX_SURFACE_SYNCHRONIZATION_V1_ERROR_NO_SURFACE,
                "the wl_surface was destroyed");
        return;
    }
    if (surface->explicit_sync.pending_release) {
        wl_resource_post_error(resource,
                ZWP_LINUX_SURFACE_SYNCHRONIZATION_V1_ERROR_DUPLICATE_RELEASE,
                "release already requested for this commit");
        return;
    }
    struct wl_resource *release_resource = wl_resource_create(client,
            &zwp_linux_buffer_release_v1_interface, 1, id);
    struct trierarch_explicit_release *release = calloc(1, sizeof(*release));
    if (!release_resource || !release) {
        if (release_resource)
            wl_resource_destroy(release_resource);
        free(release);
        wl_resource_post_no_memory(resource);
        return;
    }
    release->references = 2; /* state/frame plus wl_resource */
    release->resource = release_resource;
    wl_resource_set_implementation(release_resource, NULL, release,
            release_resource_destroy);
    surface->explicit_sync.pending_release = release;
}

static const struct zwp_linux_surface_synchronization_v1_interface sync_impl = {
    .destroy = sync_destroy,
    .set_acquire_fence = sync_set_acquire_fence,
    .get_release = sync_get_release,
};

static void explicit_sync_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void explicit_sync_get_synchronization(struct wl_client *client,
        struct wl_resource *resource, uint32_t id, struct wl_resource *surface_resource) {
    struct compositor_surface *surface = trierarch_surface_from_resource(surface_resource);
    if (surface && surface->explicit_sync.resource) {
        wl_resource_post_error(resource,
                ZWP_LINUX_EXPLICIT_SYNCHRONIZATION_V1_ERROR_SYNCHRONIZATION_EXISTS,
                "surface already has explicit synchronization");
        return;
    }
    struct wl_resource *sync_resource = wl_resource_create(client,
            &zwp_linux_surface_synchronization_v1_interface, 1, id);
    if (!sync_resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(sync_resource, &sync_impl, surface,
            sync_resource_destroy);
    if (surface)
        surface->explicit_sync.resource = sync_resource;
}

static const struct zwp_linux_explicit_synchronization_v1_interface explicit_sync_impl = {
    .destroy = explicit_sync_destroy,
    .get_synchronization = explicit_sync_get_synchronization,
};

void trierarch_explicit_sync_bind(struct wl_client *client, void *data,
        uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *resource = wl_resource_create(client,
            &zwp_linux_explicit_synchronization_v1_interface, version > 1 ? 1 : version,
            id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &explicit_sync_impl, NULL, NULL);
}
