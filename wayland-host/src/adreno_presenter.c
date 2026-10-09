#include "server_internal.h"

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <unistd.h>

#define TAG "TrierarchAdrenoPresenter"
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define TRIERARCH_ADRENO_CAPABILITIES \
    TRIERARCH_ADRENO_PRESENTER_V1_CAPABILITY_HOST_BUFFER
#define TRIERARCH_ADRENO_MAX_DIMENSION 8192
#define DRM_FORMAT_ABGR8888 0x34324241u

struct native_handle { int version; int numFds; int numInts; int data[]; };
typedef const struct native_handle *(*get_native_handle_fn)(const AHardwareBuffer *);

struct adreno_buffer_request {
    struct wl_resource *resource;
    struct wl_resource *wl_buffer;
    AHardwareBuffer *hardware_buffer;
};

static get_native_handle_fn get_native_handle(void) {
    static bool attempted;
    static get_native_handle_fn function;
    if (attempted) return function;
    attempted = true;
    function = (get_native_handle_fn)dlsym(RTLD_DEFAULT,
            "AHardwareBuffer_getNativeHandle");
    if (!function) {
        void *library = dlopen("libnativewindow.so", RTLD_NOW | RTLD_LOCAL);
        if (library)
            function = (get_native_handle_fn)dlsym(library,
                    "AHardwareBuffer_getNativeHandle");
    }
    return function;
}

static void maybe_free_request(struct adreno_buffer_request *request) {
    if (!request || request->resource || request->wl_buffer) return;
    if (request->hardware_buffer)
        AHardwareBuffer_release(request->hardware_buffer);
    free(request);
}

static void buffer_destroy(struct wl_resource *resource) {
    struct adreno_buffer_request *request = wl_resource_get_user_data(resource);
    if (!request) return;
    request->resource = NULL;
    maybe_free_request(request);
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

static void wl_buffer_destroy_request(struct wl_client *client,
        struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct wl_buffer_interface wl_buffer_impl = {
    .destroy = wl_buffer_destroy_request,
};

static void wl_buffer_destroy(struct wl_resource *resource) {
    struct adreno_buffer_request *request = wl_resource_get_user_data(resource);
    if (!request) return;
    request->wl_buffer = NULL;
    maybe_free_request(request);
}

static void buffer_fail(struct adreno_buffer_request *request, uint32_t reason) {
    if (request && request->resource)
        trierarch_adreno_buffer_v1_send_failed(request->resource, reason);
}

static bool allocate_buffer(struct wl_client *client,
        struct adreno_buffer_request *request, int32_t width, int32_t height,
        uint32_t format) {
    if (width <= 0 || height <= 0 || width > TRIERARCH_ADRENO_MAX_DIMENSION ||
            height > TRIERARCH_ADRENO_MAX_DIMENSION) {
        buffer_fail(request, TRIERARCH_ADRENO_BUFFER_V1_ERROR_INVALID_SIZE);
        return false;
    }
    if (format != DRM_FORMAT_ABGR8888) {
        buffer_fail(request, TRIERARCH_ADRENO_BUFFER_V1_ERROR_INVALID_FORMAT);
        return false;
    }
    const AHardwareBuffer_Desc description = {
        .width = (uint32_t)width,
        .height = (uint32_t)height,
        .layers = 1,
        .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
        .usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
    };
    if (AHardwareBuffer_allocate(&description, &request->hardware_buffer) != 0 ||
            !request->hardware_buffer) {
        buffer_fail(request, TRIERARCH_ADRENO_BUFFER_V1_ERROR_NO_MEMORY);
        return false;
    }
    get_native_handle_fn get_handle = get_native_handle();
    const struct native_handle *handle = get_handle
            ? get_handle(request->hardware_buffer) : NULL;
    if (!handle || handle->numFds < 1 || handle->data[0] < 0) {
        LOGW("AHardwareBuffer native handle export is unavailable");
        buffer_fail(request, TRIERARCH_ADRENO_BUFFER_V1_ERROR_UNSUPPORTED);
        return false;
    }
    AHardwareBuffer_Desc actual = {0};
    AHardwareBuffer_describe(request->hardware_buffer, &actual);
    int dma_buf = dup(handle->data[0]);
    if (dma_buf < 0) {
        buffer_fail(request, TRIERARCH_ADRENO_BUFFER_V1_ERROR_UNSUPPORTED);
        return false;
    }
    request->wl_buffer = wl_resource_create(client, &wl_buffer_interface, 1, 0);
    if (!request->wl_buffer) {
        close(dma_buf);
        buffer_fail(request, TRIERARCH_ADRENO_BUFFER_V1_ERROR_NO_MEMORY);
        return false;
    }
    wl_resource_set_implementation(request->wl_buffer, &wl_buffer_impl, request,
            wl_buffer_destroy);
    trierarch_adreno_buffer_v1_send_ready(request->resource, request->wl_buffer,
            dma_buf, actual.stride * 4u, 0, 0, 0, 0);
    return true;
}

static void presenter_destroy(struct wl_client *client,
        struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void presenter_create_buffer(struct wl_client *client,
        struct wl_resource *resource, uint32_t id, int32_t width, int32_t height,
        uint32_t format, uint32_t flags) {
    (void)resource;
    (void)flags;
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
    (void)allocate_buffer(client, request, width, height, format);
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
