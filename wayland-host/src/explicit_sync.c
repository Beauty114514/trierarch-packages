#include "server_internal.h"

#include <android/log.h>
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <unistd.h>

#define TAG "TrierarchExplicitSync"
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

/* A buffer release object is created while the commit is still pending and
 * only later attached to the buffer it releases.  Keep both owners in one
 * binding so the object's destroy hook can always clear the live pointer,
 * whichever side still references it. */
struct release_binding {
    struct compositor_surface *surface;
    struct shm_buffer *buffer;
};

static void release_destroy(struct wl_resource *resource) {
    struct release_binding *binding = wl_resource_get_user_data(resource);
    if (!binding) return;
    if (binding->surface && binding->surface->pending_explicit_release == resource)
        binding->surface->pending_explicit_release = NULL;
    if (binding->buffer && binding->buffer->explicit_release == resource)
        binding->buffer->explicit_release = NULL;
    free(binding);
}

void trierarch_explicit_sync_adopt_release(struct wl_resource *release,
        struct shm_buffer *buffer) {
    if (!release || !buffer) return;
    struct release_binding *binding = wl_resource_get_user_data(release);
    if (!binding) return;
    binding->surface = NULL;
    binding->buffer = buffer;
}

static void sync_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void sync_resource_destroy(struct wl_resource *resource) {
    struct compositor_surface *surface = wl_resource_get_user_data(resource);
    if (!surface || surface->explicit_sync != resource)
        return;
    surface->explicit_sync = NULL;
    if (surface->acquire_fence_fd >= 0) {
        close(surface->acquire_fence_fd);
        surface->acquire_fence_fd = -1;
    }
}

static void sync_set_acquire_fence(struct wl_client *client,
        struct wl_resource *resource, int32_t fd) {
    (void)client;
    struct compositor_surface *surface = wl_resource_get_user_data(resource);
    if (!surface || surface->explicit_sync != resource) {
        if (fd >= 0) close(fd);
        return;
    }
    if (surface->acquire_fence_fd >= 0) {
        close(fd);
        wl_resource_post_error(resource,
                ZWP_LINUX_SURFACE_SYNCHRONIZATION_V1_ERROR_DUPLICATE_FENCE,
                "acquire fence already set for this commit");
        return;
    }
    surface->acquire_fence_fd = fd;
}

static void sync_get_release(struct wl_client *client,
        struct wl_resource *resource, uint32_t id) {
    struct compositor_surface *surface = wl_resource_get_user_data(resource);
    if (!surface || surface->explicit_sync != resource)
        return;
    if (surface->pending_explicit_release) {
        wl_resource_post_error(resource,
                ZWP_LINUX_SURFACE_SYNCHRONIZATION_V1_ERROR_DUPLICATE_RELEASE,
                "release already set for this commit");
        return;
    }
    struct release_binding *binding = calloc(1, sizeof(*binding));
    if (!binding) {
        wl_client_post_no_memory(client);
        return;
    }
    binding->surface = surface;
    struct wl_resource *release = wl_resource_create(client,
            &zwp_linux_buffer_release_v1_interface, 1, id);
    if (!release) {
        free(binding);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(release, NULL, binding, release_destroy);
    surface->pending_explicit_release = release;
}

static const struct zwp_linux_surface_synchronization_v1_interface sync_impl = {
    .destroy = sync_destroy,
    .set_acquire_fence = sync_set_acquire_fence,
    .get_release = sync_get_release,
};

static void manager_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void manager_get_sync(struct wl_client *client, struct wl_resource *resource,
        uint32_t id, struct wl_resource *surface_resource) {
    struct compositor_surface *surface = trierarch_surface_from_resource(surface_resource);
    if (!surface || surface->explicit_sync) {
        wl_resource_post_error(resource,
                ZWP_LINUX_EXPLICIT_SYNCHRONIZATION_V1_ERROR_SYNCHRONIZATION_EXISTS,
                "surface already has explicit synchronization");
        return;
    }
    struct wl_resource *sync = wl_resource_create(client,
            &zwp_linux_surface_synchronization_v1_interface, 2, id);
    if (!sync) {
        wl_client_post_no_memory(client);
        return;
    }
    surface->explicit_sync = sync;
    if (surface->acquire_fence_fd < 0)
        surface->acquire_fence_fd = -1;
    wl_resource_set_implementation(sync, &sync_impl, surface, sync_resource_destroy);
}

static const struct zwp_linux_explicit_synchronization_v1_interface manager_impl = {
    .destroy = manager_destroy,
    .get_synchronization = manager_get_sync,
};

void trierarch_explicit_sync_bind(struct wl_client *client, void *data,
        uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client,
            &zwp_linux_explicit_synchronization_v1_interface,
            version < 2 ? version : 2, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, NULL);
}

void trierarch_explicit_sync_wait(struct shm_buffer *buffer) {
    if (!buffer || buffer->acquire_fence_fd < 0)
        return;
    struct pollfd poll_fd = { .fd = buffer->acquire_fence_fd, .events = POLLIN };
    int result;
    do {
        result = poll(&poll_fd, 1, 3000);
    } while (result < 0 && errno == EINTR);
    close(buffer->acquire_fence_fd);
    buffer->acquire_fence_fd = -1;
    if (result <= 0)
        LOGW("acquire fence did not signal: result=%d", result);
}

void trierarch_explicit_sync_buffer_release(struct shm_buffer *buffer) {
    if (!buffer || !buffer->explicit_release)
        return;
    struct wl_resource *release = buffer->explicit_release;
    buffer->explicit_release = NULL;
    zwp_linux_buffer_release_v1_send_immediate_release(release);
    wl_resource_destroy(release);
}
