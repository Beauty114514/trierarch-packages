#include "server_internal.h"
#include <android/hardware_buffer.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* These globals are intentionally minimal. They make the host look like the
 * old Trierarch compositor during registry negotiation; the renderer still
 * uses SHM/EGL buffers and does not depend on their optional data paths. */
static void destroy_resource(struct wl_client *client, struct wl_resource *resource) {
    (void)client; wl_resource_destroy(resource);
}
static void fractional_destroy(struct wl_client *, struct wl_resource *);

static void fractional_get(struct wl_client *client, struct wl_resource *manager,
        uint32_t id, struct wl_resource *surface) {
    struct compositor_surface *compositor_surface = trierarch_surface_from_resource(surface);
    if (!compositor_surface || compositor_surface->fractional_scale) {
        wl_resource_post_error(manager,
                WP_FRACTIONAL_SCALE_MANAGER_V1_ERROR_FRACTIONAL_SCALE_EXISTS,
                "fractional scale already exists");
        return;
    }
    struct wl_resource *resource = wl_resource_create(client,
            &wp_fractional_scale_v1_interface, 1, id);
    if (!resource) { wl_client_post_no_memory(client); return; }
    static const struct wp_fractional_scale_v1_interface fractional_surface_impl = {
        .destroy = fractional_destroy,
    };
    compositor_surface->fractional_scale = resource;
    wl_resource_set_implementation(resource, &fractional_surface_impl,
            compositor_surface, NULL);
    wp_fractional_scale_v1_send_preferred_scale(resource, 120);
}
static void fractional_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    struct compositor_surface *surface = wl_resource_get_user_data(resource);
    if (surface && surface->fractional_scale == resource)
        surface->fractional_scale = NULL;
    wl_resource_destroy(resource);
}
static const struct wp_fractional_scale_manager_v1_interface fractional_impl = {
    .destroy = destroy_resource, .get_fractional_scale = fractional_get,
};
void trierarch_fractional_scale_bind(struct wl_client *client, void *data,
        uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client,
            &wp_fractional_scale_manager_v1_interface, version < 1 ? version : 1, id);
    if (!resource) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(resource, &fractional_impl, data, NULL);
}

static void relative_get(struct wl_client *client, struct wl_resource *manager,
        uint32_t id, struct wl_resource *pointer) {
    (void)manager; (void)pointer;
    struct wl_resource *resource = wl_resource_create(client,
            &zwp_relative_pointer_v1_interface, 1, id);
    if (!resource) { wl_client_post_no_memory(client); return; }
    static const struct zwp_relative_pointer_v1_interface relative_pointer_impl = {
        .destroy = destroy_resource,
    };
    wl_resource_set_implementation(resource, &relative_pointer_impl, NULL, NULL);
}
static const struct zwp_relative_pointer_manager_v1_interface relative_impl = {
    .destroy = destroy_resource, .get_relative_pointer = relative_get,
};
void trierarch_relative_pointer_bind(struct wl_client *client, void *data,
        uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client,
            &zwp_relative_pointer_manager_v1_interface, version < 1 ? version : 1, id);
    if (!resource) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(resource, &relative_impl, data, NULL);
}

typedef struct native_handle native_handle_t;
typedef int (*create_ahb_from_handle_fn)(const AHardwareBuffer_Desc *,
        const native_handle_t *, int32_t, AHardwareBuffer **);

struct native_handle {
    int version;
    int numFds;
    int numInts;
    int data[];
};

struct wlegl_handle {
    int expected_fds;
    int received_fds;
    int *fds;
    int *ints;
    size_t ints_count;
};

static create_ahb_from_handle_fn create_ahb_from_handle;

static create_ahb_from_handle_fn get_create_ahb_from_handle(void) {
    if (create_ahb_from_handle) return create_ahb_from_handle;
    void *library = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) return NULL;
    create_ahb_from_handle = (create_ahb_from_handle_fn)dlsym(library,
            "AHardwareBuffer_createFromHandle");
    return create_ahb_from_handle;
}

static void wlegl_handle_add_fd(struct wl_client *client, struct wl_resource *resource,
        int32_t fd) {
    (void)client;
    struct wlegl_handle *handle = wl_resource_get_user_data(resource);
    if (!handle || handle->received_fds >= handle->expected_fds) {
        if (fd >= 0) close(fd);
        return;
    }
    handle->fds[handle->received_fds++] = fd;
}
static const struct android_wlegl_handle_interface handle_impl = {
    .add_fd = wlegl_handle_add_fd, .destroy = destroy_resource,
};

static void wlegl_handle_destroy(struct wl_resource *resource) {
    struct wlegl_handle *handle = wl_resource_get_user_data(resource);
    if (!handle) return;
    for (int i = 0; i < handle->received_fds; ++i)
        if (handle->fds[i] >= 0) close(handle->fds[i]);
    free(handle->fds);
    free(handle->ints);
    free(handle);
}

static void android_buffer_destroy_request(struct wl_client *client,
        struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct wl_buffer_interface android_buffer_impl = {
    .destroy = android_buffer_destroy_request,
};

static void android_buffer_destroy(struct wl_resource *resource) {
    struct shm_buffer *buffer = wl_resource_get_user_data(resource);
    if (!buffer) return;
    buffer->resource = NULL;
    if (buffer->android_hardware_buffer)
        AHardwareBuffer_release(buffer->android_hardware_buffer);
    free(buffer);
}

struct shm_buffer *trierarch_android_buffer_from_resource(struct wl_resource *resource) {
    if (!resource || !wl_resource_instance_of(resource, &wl_buffer_interface,
            &android_buffer_impl)) return NULL;
    struct shm_buffer *buffer = wl_resource_get_user_data(resource);
    return buffer && buffer->android_buffer ? buffer : NULL;
}

static void wlegl_create_handle(struct wl_client *client, struct wl_resource *resource,
        uint32_t id, int32_t num_fds, struct wl_array *ints) {
    (void)resource;
    if (num_fds < 0) { wl_client_post_no_memory(client); return; }
    struct wlegl_handle *state = calloc(1, sizeof(*state));
    if (!state) { wl_client_post_no_memory(client); return; }
    state->expected_fds = num_fds;
    state->fds = calloc((size_t)num_fds, sizeof(*state->fds));
    state->ints_count = ints ? ints->size / sizeof(*state->ints) : 0;
    state->ints = calloc(state->ints_count, sizeof(*state->ints));
    if ((num_fds && !state->fds) || (state->ints_count && !state->ints)) {
        free(state->fds); free(state->ints); free(state);
        wl_client_post_no_memory(client); return;
    }
    for (int i = 0; i < num_fds; ++i) state->fds[i] = -1;
    if (state->ints_count) memcpy(state->ints, ints->data,
            state->ints_count * sizeof(*state->ints));
    struct wl_resource *handle = wl_resource_create(client,
            &android_wlegl_handle_interface, 1, id);
    if (!handle) {
        free(state->fds); free(state->ints); free(state);
        wl_client_post_no_memory(client); return;
    }
    wl_resource_set_implementation(handle, &handle_impl, state, wlegl_handle_destroy);
}
static void wlegl_create_buffer(struct wl_client *client, struct wl_resource *resource,
        uint32_t id, int32_t width, int32_t height, int32_t stride, int32_t format,
        int32_t usage, struct wl_resource *handle) {
    struct wlegl_handle *state = wl_resource_get_user_data(handle);
    if (!state || state->received_fds != state->expected_fds || width <= 0 ||
            height <= 0 || stride <= 0 || state->expected_fds == 0) return;
    uint32_t ahb_format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    if ((uint32_t)format != 0x34325258u && (uint32_t)format != 0x34325241u &&
            (uint32_t)format != 0x34324142u) return;
    size_t handle_size = sizeof(native_handle_t) +
            (size_t)(state->expected_fds + state->ints_count) * sizeof(int);
    native_handle_t *native = calloc(1, handle_size);
    if (!native) { wl_client_post_no_memory(client); return; }
    native->version = sizeof(*native);
    native->numFds = state->expected_fds;
    native->numInts = (int)state->ints_count;
    memcpy(native->data, state->fds, (size_t)state->expected_fds * sizeof(int));
    if (state->ints_count) memcpy(native->data + state->expected_fds, state->ints,
            state->ints_count * sizeof(int));
    AHardwareBuffer_Desc description = {
        .width = (uint32_t)width, .height = (uint32_t)height, .layers = 1,
        .format = ahb_format, .usage = (uint64_t)(uint32_t)usage,
        .stride = (uint32_t)stride,
    };
    AHardwareBuffer *hardware_buffer = NULL;
    create_ahb_from_handle_fn create = get_create_ahb_from_handle();
    int result = create ? create(&description, native, 0, &hardware_buffer) : -1;
    free(native);
    if (result != 0 || !hardware_buffer) return;
    struct shm_buffer *buffer = calloc(1, sizeof(*buffer));
    if (!buffer) {
        AHardwareBuffer_release(hardware_buffer);
        wl_client_post_no_memory(client); return;
    }
    buffer->android_buffer = true;
    buffer->android_hardware_buffer = hardware_buffer;
    buffer->width = width;
    buffer->height = height;
    buffer->stride = stride;
    buffer->format = (uint32_t)format;
    buffer->resource = wl_resource_create(client, &wl_buffer_interface, 1, id);
    if (!buffer->resource) {
        AHardwareBuffer_release(hardware_buffer); free(buffer);
        wl_client_post_no_memory(client); return;
    }
    wl_resource_set_implementation(buffer->resource, &android_buffer_impl, buffer,
            android_buffer_destroy);
}
static const struct android_wlegl_interface wlegl_impl = {
    .create_handle = wlegl_create_handle, .create_buffer = wlegl_create_buffer,
};
void trierarch_android_wlegl_bind(struct wl_client *client, void *data,
        uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client,
            &android_wlegl_interface, version < 1 ? version : 1, id);
    if (!resource) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(resource, &wlegl_impl, data, NULL);
}

static void data_device_release(struct wl_client *client, struct wl_resource *resource) {
    (void)client; wl_resource_destroy(resource);
}
static void data_device_start_drag(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *source, struct wl_resource *origin, struct wl_resource *icon,
        uint32_t serial) {
    (void)client; (void)resource; (void)source; (void)origin; (void)icon; (void)serial;
}
static void data_device_set_selection(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *source, uint32_t serial) {
    (void)client; (void)resource; (void)source; (void)serial;
}
static const struct wl_data_device_interface data_device_impl = {
    .release = data_device_release,
    .start_drag = data_device_start_drag,
    .set_selection = data_device_set_selection,
};
static void data_source_offer(struct wl_client *client, struct wl_resource *resource,
        const char *mime) { (void)client; (void)resource; (void)mime; }
static void data_source_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client; wl_resource_destroy(resource);
}
static void data_source_set_actions(struct wl_client *client, struct wl_resource *resource,
        uint32_t actions) { (void)client; (void)resource; (void)actions; }
static const struct wl_data_source_interface data_source_impl = {
    .offer = data_source_offer,
    .destroy = data_source_destroy,
    .set_actions = data_source_set_actions,
};
static void data_create_source(struct wl_client *client, struct wl_resource *manager,
        uint32_t id) {
    struct wl_resource *source = wl_resource_create(client, &wl_data_source_interface,
            wl_resource_get_version(manager), id);
    if (!source) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(source, &data_source_impl, NULL, NULL);
}
static void data_get_device(struct wl_client *client, struct wl_resource *manager,
        uint32_t id, struct wl_resource *seat) {
    (void)seat;
    struct wl_resource *device = wl_resource_create(client, &wl_data_device_interface,
            wl_resource_get_version(manager), id);
    if (!device) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(device, &data_device_impl, NULL, NULL);
}
static const struct wl_data_device_manager_interface data_manager_impl = {
    .create_data_source = data_create_source,
    .get_data_device = data_get_device,
};
void trierarch_data_device_bind(struct wl_client *client, void *data,
        uint32_t version, uint32_t id) {
    struct wl_resource *manager = wl_resource_create(client,
            &wl_data_device_manager_interface, version < 3 ? version : 3, id);
    if (!manager) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(manager, &data_manager_impl, data, NULL);
}
