#include "server_internal.h"

#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#define TAG "TrierarchWayland"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

#define DRM_FORMAT_XRGB8888 0x34325258u
#define DRM_FORMAT_ARGB8888 0x34325241u
#define DRM_FORMAT_XBGR8888 0x34324258u
#define DRM_FORMAT_ABGR8888 0x34324241u
#define DRM_FORMAT_MOD_INVALID 0x00ffffffffffffffULL

#ifndef SYS_memfd_create
#if defined(__aarch64__)
#define SYS_memfd_create 279
#elif defined(__arm__)
#define SYS_memfd_create 385
#endif
#endif

struct dmabuf_params {
    int fd;
    uint32_t offset;
    uint32_t stride;
    uint64_t modifier;
    bool added;
    bool used;
};

static bool supported_format(uint32_t format) {
    return format == DRM_FORMAT_XRGB8888 || format == DRM_FORMAT_ARGB8888 ||
            format == DRM_FORMAT_XBGR8888 || format == DRM_FORMAT_ABGR8888;
}

static void dmabuf_buffer_destroy(struct wl_resource *resource) {
    struct shm_buffer *buffer = wl_resource_get_user_data(resource);
    if (!buffer) return;
    buffer->resource = NULL;
    if (buffer->dmabuf_mapping)
        munmap(buffer->dmabuf_mapping, buffer->dmabuf_mapping_size);
    if (buffer->dmabuf_fd >= 0)
        close(buffer->dmabuf_fd);
    free(buffer);
}

static void dmabuf_buffer_destroy_request(struct wl_client *client,
        struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct wl_buffer_interface dmabuf_buffer_impl = {
    .destroy = dmabuf_buffer_destroy_request,
};

static struct shm_buffer *make_dmabuf_buffer(struct wl_client *client,
        int id, int32_t width, int32_t height, uint32_t format,
        struct dmabuf_params *params) {
    if (!params->added || params->fd < 0 || width <= 0 || height <= 0 ||
            !supported_format(format) || params->stride < (uint32_t)width * 4) {
        return NULL;
    }
    size_t end = (size_t)params->offset + (size_t)params->stride * (size_t)height;
    void *mapping = mmap(NULL, end, PROT_READ, MAP_SHARED, params->fd, 0);
    if (mapping == MAP_FAILED) {
        LOGW("dmabuf CPU fallback mmap failed: fd=%d size=%zu errno=%d",
                params->fd, end, errno);
        mapping = NULL;
    }
    struct shm_buffer *buffer = calloc(1, sizeof(*buffer));
    if (!buffer) {
        if (mapping) munmap(mapping, end);
        close(params->fd);
        params->fd = -1;
        return NULL;
    }
    buffer->dmabuf = true;
    buffer->dmabuf_fd = params->fd;
    buffer->dmabuf_mapping = mapping;
    buffer->dmabuf_mapping_size = mapping ? end : 0;
    buffer->data = mapping ? (char *)mapping + params->offset : NULL;
    buffer->size = (size_t)params->stride * (size_t)height;
    buffer->width = width;
    buffer->height = height;
    buffer->stride = (int32_t)params->stride;
    buffer->format = format;
    buffer->dmabuf_modifier = params->modifier;
    buffer->dmabuf_offset = params->offset;
    buffer->dmabuf_fd = params->fd;
    params->fd = -1;
    buffer->resource = wl_resource_create(client, &wl_buffer_interface, 1, id);
    if (!buffer->resource) {
        if (mapping) munmap(mapping, end);
        close(buffer->dmabuf_fd);
        free(buffer);
        return NULL;
    }
    wl_resource_set_implementation(buffer->resource, &dmabuf_buffer_impl,
            buffer, dmabuf_buffer_destroy);
    LOGI("dmabuf accepted: %dx%d fmt=0x%x stride=%u mod=0x%llx cpu-fallback=%s",
            width, height, format, params->stride,
            (unsigned long long)params->modifier, mapping ? "ready" : "unavailable");
    return buffer;
}

static void params_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void params_add(struct wl_client *client, struct wl_resource *resource,
        int32_t fd, uint32_t plane, uint32_t offset, uint32_t stride,
        uint32_t modifier_hi, uint32_t modifier_lo) {
    (void)client;
    struct dmabuf_params *params = wl_resource_get_user_data(resource);
    if (!params || params->used || params->added || plane != 0 || fd < 0) {
        if (fd >= 0) close(fd);
        if (params) wl_resource_post_error(resource,
                ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED, "invalid parameters");
        return;
    }
    params->fd = fd;
    params->offset = offset;
    params->stride = stride;
    params->modifier = ((uint64_t)modifier_hi << 32) | modifier_lo;
    params->added = true;
}

static void params_create(struct wl_client *client, struct wl_resource *resource,
        int32_t width, int32_t height, uint32_t format, uint32_t flags) {
    (void)flags;
    struct dmabuf_params *params = wl_resource_get_user_data(resource);
    if (!params || params->used) return;
    struct shm_buffer *buffer = make_dmabuf_buffer(client, 0, width, height, format, params);
    params->used = true;
    if (!buffer) {
        zwp_linux_buffer_params_v1_send_failed(resource);
        return;
    }
    zwp_linux_buffer_params_v1_send_created(resource, buffer->resource);
}

static void params_create_immed(struct wl_client *client, struct wl_resource *resource,
        uint32_t buffer_id, int32_t width, int32_t height, uint32_t format, uint32_t flags) {
    (void)flags;
    struct dmabuf_params *params = wl_resource_get_user_data(resource);
    if (!params || params->used) return;
    struct shm_buffer *buffer = make_dmabuf_buffer(client, (int)buffer_id,
            width, height, format, params);
    params->used = true;
    if (!buffer)
        wl_resource_post_error(resource,
                ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE, "unsupported dmabuf parameters");
}

static void params_resource_destroy(struct wl_resource *resource) {
    struct dmabuf_params *params = wl_resource_get_user_data(resource);
    if (!params) return;
    if (params->fd >= 0) close(params->fd);
    free(params);
}

static const struct zwp_linux_buffer_params_v1_interface params_impl = {
    .destroy = params_destroy,
    .add = params_add,
    .create = params_create,
    .create_immed = params_create_immed,
};

static void dmabuf_create_params(struct wl_client *client, struct wl_resource *resource,
        uint32_t id) {
    struct dmabuf_params *params = calloc(1, sizeof(*params));
    if (!params) {
        wl_client_post_no_memory(client);
        return;
    }
    params->fd = -1;
    struct wl_resource *params_resource = wl_resource_create(client,
            &zwp_linux_buffer_params_v1_interface, wl_resource_get_version(resource), id);
    if (!params_resource) {
        free(params);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(params_resource, &params_impl, params,
            params_resource_destroy);
}

static void feedback_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct zwp_linux_dmabuf_feedback_v1_interface feedback_impl = {
    .destroy = feedback_destroy,
};

static void send_feedback(struct wl_resource *resource) {
    struct {
        uint32_t format;
        uint32_t padding;
        uint64_t modifier;
    } entries[8] = {
        { DRM_FORMAT_XRGB8888, 0, 0 },
        { DRM_FORMAT_ARGB8888, 0, 0 },
        { DRM_FORMAT_XBGR8888, 0, 0 },
        { DRM_FORMAT_ABGR8888, 0, 0 },
        { DRM_FORMAT_XRGB8888, 0, DRM_FORMAT_MOD_INVALID },
        { DRM_FORMAT_ARGB8888, 0, DRM_FORMAT_MOD_INVALID },
        { DRM_FORMAT_XBGR8888, 0, DRM_FORMAT_MOD_INVALID },
        { DRM_FORMAT_ABGR8888, 0, DRM_FORMAT_MOD_INVALID },
    };
#ifdef SYS_memfd_create
    int fd = (int)syscall(SYS_memfd_create, "trierarch-dmabuf-feedback", 0);
    if (fd >= 0 && ftruncate(fd, (off_t)sizeof(entries)) == 0 &&
            write(fd, entries, sizeof(entries)) == (ssize_t)sizeof(entries)) {
        lseek(fd, 0, SEEK_SET);
        zwp_linux_dmabuf_feedback_v1_send_format_table(resource, fd,
                (uint32_t)sizeof(entries));
        close(fd);
    } else if (fd >= 0) {
        close(fd);
    }
#endif
    struct wl_array device;
    wl_array_init(&device);
    dev_t zero = 0;
    void *device_data = wl_array_add(&device, sizeof(zero));
    if (device_data) memcpy(device_data, &zero, sizeof(zero));
    zwp_linux_dmabuf_feedback_v1_send_main_device(resource, &device);
    zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(resource, &device);
    zwp_linux_dmabuf_feedback_v1_send_tranche_flags(resource, 0);
    struct wl_array indices;
    wl_array_init(&indices);
    uint16_t *index_data = wl_array_add(&indices, sizeof(uint16_t) * 8);
    if (index_data) {
        for (uint16_t i = 0; i < 8; ++i) index_data[i] = i;
        zwp_linux_dmabuf_feedback_v1_send_tranche_formats(resource, &indices);
    }
    zwp_linux_dmabuf_feedback_v1_send_tranche_done(resource);
    zwp_linux_dmabuf_feedback_v1_send_done(resource);
    wl_array_release(&indices);
    wl_array_release(&device);
}

static void dmabuf_get_default_feedback(struct wl_client *client,
        struct wl_resource *resource, uint32_t id) {
    (void)resource;
    struct wl_resource *feedback = wl_resource_create(client,
            &zwp_linux_dmabuf_feedback_v1_interface, 1, id);
    if (!feedback) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(feedback, &feedback_impl, NULL, NULL);
    send_feedback(feedback);
}

static void dmabuf_get_surface_feedback(struct wl_client *client,
        struct wl_resource *resource, uint32_t id, struct wl_resource *surface) {
    (void)surface;
    dmabuf_get_default_feedback(client, resource, id);
}

static void dmabuf_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct zwp_linux_dmabuf_v1_interface dmabuf_impl = {
    .destroy = dmabuf_destroy,
    .create_params = dmabuf_create_params,
    .get_default_feedback = dmabuf_get_default_feedback,
    .get_surface_feedback = dmabuf_get_surface_feedback,
};

void trierarch_dmabuf_bind(struct wl_client *client, void *data,
        uint32_t version, uint32_t id) {
    (void)data;
    if (version > 4) version = 4;
    struct wl_resource *resource = wl_resource_create(client,
            &zwp_linux_dmabuf_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &dmabuf_impl, NULL, NULL);
    zwp_linux_dmabuf_v1_send_format(resource, DRM_FORMAT_XRGB8888);
    zwp_linux_dmabuf_v1_send_format(resource, DRM_FORMAT_ARGB8888);
    zwp_linux_dmabuf_v1_send_format(resource, DRM_FORMAT_XBGR8888);
    zwp_linux_dmabuf_v1_send_format(resource, DRM_FORMAT_ABGR8888);
    if (version >= 3) {
        zwp_linux_dmabuf_v1_send_modifier(resource, DRM_FORMAT_XRGB8888, 0, 0);
        zwp_linux_dmabuf_v1_send_modifier(resource, DRM_FORMAT_ARGB8888, 0, 0);
        zwp_linux_dmabuf_v1_send_modifier(resource, DRM_FORMAT_XBGR8888, 0, 0);
        zwp_linux_dmabuf_v1_send_modifier(resource, DRM_FORMAT_ABGR8888, 0, 0);
        zwp_linux_dmabuf_v1_send_modifier(resource, DRM_FORMAT_XRGB8888,
                (uint32_t)(DRM_FORMAT_MOD_INVALID >> 32), (uint32_t)DRM_FORMAT_MOD_INVALID);
        zwp_linux_dmabuf_v1_send_modifier(resource, DRM_FORMAT_ARGB8888,
                (uint32_t)(DRM_FORMAT_MOD_INVALID >> 32), (uint32_t)DRM_FORMAT_MOD_INVALID);
        zwp_linux_dmabuf_v1_send_modifier(resource, DRM_FORMAT_XBGR8888,
                (uint32_t)(DRM_FORMAT_MOD_INVALID >> 32), (uint32_t)DRM_FORMAT_MOD_INVALID);
        zwp_linux_dmabuf_v1_send_modifier(resource, DRM_FORMAT_ABGR8888,
                (uint32_t)(DRM_FORMAT_MOD_INVALID >> 32), (uint32_t)DRM_FORMAT_MOD_INVALID);
    }
    LOGI("linux-dmabuf bound at version %u", version);
}

struct shm_buffer *trierarch_dmabuf_buffer_from_resource(struct wl_resource *resource) {
    if (!resource) return NULL;
    struct shm_buffer *buffer = wl_resource_get_user_data(resource);
    return buffer && buffer->dmabuf ? buffer : NULL;
}

void trierarch_dmabuf_buffer_release(struct shm_buffer *buffer) {
    if (!buffer || !buffer->resource || !buffer->busy) return;
    buffer->busy = false;
    wl_buffer_send_release(buffer->resource);
}
