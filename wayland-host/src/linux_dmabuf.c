#include "server_internal.h"
#include "dmabuf_feedback_device.h"

#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#define TAG "TrierarchWayland"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

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
    return format == TRIERARCH_DRM_FORMAT_XRGB8888 ||
            format == TRIERARCH_DRM_FORMAT_ARGB8888 ||
            format == TRIERARCH_DRM_FORMAT_XBGR8888 ||
            format == TRIERARCH_DRM_FORMAT_ABGR8888;
}

static void dmabuf_buffer_destroy(struct wl_resource *resource) {
    struct shm_buffer *buffer = wl_resource_get_user_data(resource);
    if (!buffer) return;
    buffer->resource = NULL;
    trierarch_buffer_unref(buffer);
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
            !supported_format(format) || width > INT32_MAX / 4 ||
            params->stride < (uint32_t)width * 4 ||
            params->stride > INT32_MAX) {
        return NULL;
    }
    size_t end = 0;
    void *mapping = NULL;
    end = (size_t)params->offset + (size_t)params->stride * (size_t)height;
    mapping = mmap(NULL, end, PROT_READ, MAP_SHARED, params->fd, 0);
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
    buffer->references = 1;
    buffer->destroy = trierarch_buffer_destroy_storage;
    buffer->dmabuf = true;
    buffer->acquire_fence_fd = -1;
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
    LOGI("dmabuf accepted: %dx%d fmt=0x%x stride=%u mod=0x%llx source=%s",
            width, height, format, params->stride,
            (unsigned long long)params->modifier,
            mapping ? "cpu-mappable" : "opaque");
    return buffer;
}

static void params_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void params_add(struct wl_client *client, struct wl_resource *resource,
        int32_t fd, uint32_t plane, uint32_t offset, uint32_t stride,
        uint32_t modifier_hi, uint32_t modifier_lo) {
    struct dmabuf_params *params = wl_resource_get_user_data(resource);
    static unsigned int trace_count;
    if (trace_count++ < 64) {
        pid_t pid = -1;
        uid_t uid = (uid_t)-1;
        gid_t gid = (gid_t)-1;
        if (client) wl_client_get_credentials(client, &pid, &uid, &gid);
        LOGI("dmabuf trace request=params_add pid=%d uid=%u params=%u fd=%d "
                "plane=%u offset=%u stride=%u modifier=0x%08x%08x",
                (int)pid, (unsigned int)uid, wl_resource_get_id(resource), fd,
                plane, offset, stride, modifier_hi, modifier_lo);
    }
    if (!params || params->used || params->added || plane != 0 || fd < 0) {
        LOGW("dmabuf trace result=params_add rejected params=%u used=%d added=%d "
                "plane=%u fd=%d", wl_resource_get_id(resource),
                params ? params->used : 0, params ? params->added : 0, plane, fd);
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
    static unsigned int trace_count;
    bool trace = trace_count++ < 64;
    pid_t pid = -1;
    uid_t uid = (uid_t)-1;
    gid_t gid = (gid_t)-1;
    if (client) wl_client_get_credentials(client, &pid, &uid, &gid);
    if (trace)
        LOGI("dmabuf trace request=create pid=%d uid=%u params=%u size=%dx%d "
                "fmt=0x%x added=%d", (int)pid, (unsigned int)uid,
                wl_resource_get_id(resource), width, height, format, params->added);
    struct shm_buffer *buffer = make_dmabuf_buffer(client, 0, width, height, format, params);
    params->used = true;
    if (!buffer) {
        if (trace)
            LOGW("dmabuf trace result=create failed params=%u size=%dx%d fmt=0x%x",
                    wl_resource_get_id(resource), width, height, format);
        zwp_linux_buffer_params_v1_send_failed(resource);
        return;
    }
    if (trace)
        LOGI("dmabuf trace result=create succeeded buffer=%u",
                wl_resource_get_id(buffer->resource));
    zwp_linux_buffer_params_v1_send_created(resource, buffer->resource);
}

static void params_create_immed(struct wl_client *client, struct wl_resource *resource,
        uint32_t buffer_id, int32_t width, int32_t height, uint32_t format, uint32_t flags) {
    (void)flags;
    struct dmabuf_params *params = wl_resource_get_user_data(resource);
    if (!params || params->used) return;
    static unsigned int trace_count;
    bool trace = trace_count++ < 64;
    pid_t pid = -1;
    uid_t uid = (uid_t)-1;
    gid_t gid = (gid_t)-1;
    if (client) wl_client_get_credentials(client, &pid, &uid, &gid);
    if (trace)
        LOGI("dmabuf trace request=create_immed pid=%d uid=%u params=%u buffer=%u "
                "size=%dx%d fmt=0x%x added=%d", (int)pid, (unsigned int)uid,
                wl_resource_get_id(resource), buffer_id, width, height, format,
                params->added);
    struct shm_buffer *buffer = make_dmabuf_buffer(client, (int)buffer_id,
            width, height, format, params);
    params->used = true;
    if (!buffer) {
        if (trace)
            LOGW("dmabuf trace result=create_immed failed params=%u buffer=%u "
                    "size=%dx%d fmt=0x%x", wl_resource_get_id(resource),
                    buffer_id, width, height, format);
        wl_resource_post_error(resource,
                ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE, "unsupported dmabuf parameters");
    } else {
        if (trace)
            LOGI("dmabuf trace result=create_immed succeeded buffer=%u", buffer_id);
    }
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

static void send_feedback(struct wl_resource *resource, dev_t main_device) {
    struct {
        uint32_t format;
        uint32_t padding;
        uint64_t modifier;
    } entries[4] = {
        { TRIERARCH_DRM_FORMAT_XRGB8888, 0, 0 },
        { TRIERARCH_DRM_FORMAT_ARGB8888, 0, 0 },
        { TRIERARCH_DRM_FORMAT_XBGR8888, 0, 0 },
        { TRIERARCH_DRM_FORMAT_ABGR8888, 0, 0 },
    };
    struct wl_client *client = wl_resource_get_client(resource);
    pid_t client_pid = -1;
    uid_t client_uid = (uid_t)-1;
    gid_t client_gid = (gid_t)-1;
    if (client) wl_client_get_credentials(client, &client_pid, &client_uid, &client_gid);
    bool format_table_prepared = false;
#ifdef SYS_memfd_create
    int fd = (int)syscall(SYS_memfd_create, "trierarch-dmabuf-feedback", 0);
    if (fd < 0) {
        LOGW("dmabuf trace feedback memfd-create failed errno=%d", errno);
    } else if (ftruncate(fd, (off_t)sizeof(entries)) != 0) {
        LOGW("dmabuf trace feedback ftruncate failed fd=%d errno=%d", fd, errno);
        close(fd);
    } else {
        ssize_t written = write(fd, entries, sizeof(entries));
        if (written != (ssize_t)sizeof(entries)) {
            LOGW("dmabuf trace feedback write failed fd=%d bytes=%zd expected=%zu errno=%d",
                    fd, written, sizeof(entries), errno);
            close(fd);
        } else {
            off_t offset = lseek(fd, 0, SEEK_SET);
            if (offset != 0) {
                LOGW("dmabuf trace feedback rewind failed fd=%d offset=%lld errno=%d",
                        fd, (long long)offset, errno);
                close(fd);
            } else {
                errno = 0;
                int flags_before = fcntl(fd, F_GETFD);
                int errno_before = errno;
                errno = 0;
                int duplicate_probe = fcntl(fd, F_DUPFD_CLOEXEC, 0);
                int duplicate_errno = errno;
                if (duplicate_probe >= 0) close(duplicate_probe);
                LOGI("dmabuf trace feedback event=before-format-table client-pid=%d uid=%u "
                        "fd=%d flags=%d errno=%d dup-probe=%d dup-errno=%d size=%zu",
                        (int)client_pid, (unsigned int)client_uid, fd, flags_before,
                        errno_before, duplicate_probe, duplicate_errno, sizeof(entries));
                if (flags_before >= 0) {
                    zwp_linux_dmabuf_feedback_v1_send_format_table(resource, fd,
                            (uint32_t)sizeof(entries));
                    errno = 0;
                    int flags_after = fcntl(fd, F_GETFD);
                    int errno_after = errno;
                    LOGI("dmabuf trace feedback event=after-format-table client-pid=%d "
                            "fd=%d flags=%d errno=%d",
                            (int)client_pid, fd, flags_after, errno_after);
                    format_table_prepared = true;
                }
                close(fd);
            }
        }
    }
#endif
    if (!format_table_prepared)
        LOGW("dmabuf trace feedback format-table not prepared for event");
    struct wl_array device;
    wl_array_init(&device);
    void *device_data = wl_array_add(&device, sizeof(main_device));
    if (device_data) memcpy(device_data, &main_device, sizeof(main_device));
    zwp_linux_dmabuf_feedback_v1_send_main_device(resource, &device);
    zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(resource, &device);
    zwp_linux_dmabuf_feedback_v1_send_tranche_flags(resource, 0);
    struct wl_array indices;
    wl_array_init(&indices);
    uint16_t *index_data = wl_array_add(&indices, sizeof(uint16_t) * 4);
    if (index_data) {
        for (uint16_t i = 0; i < 4; ++i) index_data[i] = i;
        zwp_linux_dmabuf_feedback_v1_send_tranche_formats(resource, &indices);
    }
    zwp_linux_dmabuf_feedback_v1_send_tranche_done(resource);
    zwp_linux_dmabuf_feedback_v1_send_done(resource);
    LOGI("dmabuf trace feedback metadata queued table-prepared=%d tranches=1 done=1; event API has no send result",
            format_table_prepared);
    wl_array_release(&indices);
    wl_array_release(&device);
}

static void dmabuf_get_feedback(struct wl_client *client,
        struct wl_resource *resource, uint32_t id, const char *kind) {
    struct wayland_server *server = wl_resource_get_user_data(resource);
    pid_t pid = -1;
    uid_t uid = (uid_t)-1;
    gid_t gid = (gid_t)-1;
    if (client) wl_client_get_credentials(client, &pid, &uid, &gid);
    LOGI("dmabuf trace request=get_%s_feedback pid=%d uid=%u version=%u id=%u",
            kind, (int)pid, (unsigned int)uid,
            wl_resource_get_version(resource), id);
    dev_t main_device;
    if (!trierarch_dmabuf_feedback_device_get(server->dmabuf_feedback_device,
            &main_device)) {
        LOGW("dmabuf trace result=feedback unavailable id=%u", id);
        wl_client_post_implementation_error(client,
                "linux-dmabuf v4 global has no registered feedback device");
        return;
    }
    struct wl_resource *feedback = wl_resource_create(client,
            &zwp_linux_dmabuf_feedback_v1_interface, 1, id);
    if (!feedback) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(feedback, &feedback_impl, NULL, NULL);
    send_feedback(feedback, main_device);
    LOGI("dmabuf trace result=feedback sent kind=%s id=%u main-device=%llu formats=4",
            kind, id, (unsigned long long)main_device);
}

static void dmabuf_get_default_feedback(struct wl_client *client,
        struct wl_resource *resource, uint32_t id) {
    dmabuf_get_feedback(client, resource, id, "default");
}

static void dmabuf_get_surface_feedback(struct wl_client *client,
        struct wl_resource *resource, uint32_t id, struct wl_resource *surface) {
    (void)surface;
    dmabuf_get_feedback(client, resource, id, "surface");
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

bool trierarch_dmabuf_global_create(struct wayland_server *server) {
    if (!server || !server->display) return false;
    if (server->dmabuf_global) return true;
    dev_t main_device;
    if (!trierarch_dmabuf_feedback_device_get(server->dmabuf_feedback_device,
            &main_device)) {
        LOGW("refusing to publish linux-dmabuf v4 global without a feedback device");
        return false;
    }

    server->dmabuf_global = wl_global_create(server->display,
            &zwp_linux_dmabuf_v1_interface, 4, server, trierarch_dmabuf_bind);
    if (!server->dmabuf_global) {
        LOGW("unable to publish linux-dmabuf v4 global after device report");
        return false;
    }
    LOGI("published linux-dmabuf v4 global after guest device report");
    return true;
}

void trierarch_dmabuf_bind(struct wl_client *client, void *data,
        uint32_t version, uint32_t id) {
    struct wayland_server *server = data;
    if (version > 4) version = 4;
    struct wl_resource *resource = wl_resource_create(client,
            &zwp_linux_dmabuf_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &dmabuf_impl, server, NULL);
    if (version < 4) {
        zwp_linux_dmabuf_v1_send_format(resource, TRIERARCH_DRM_FORMAT_XRGB8888);
        zwp_linux_dmabuf_v1_send_format(resource, TRIERARCH_DRM_FORMAT_ARGB8888);
        zwp_linux_dmabuf_v1_send_format(resource, TRIERARCH_DRM_FORMAT_XBGR8888);
        zwp_linux_dmabuf_v1_send_format(resource, TRIERARCH_DRM_FORMAT_ABGR8888);
    }
    if (version >= 3 && version < 4) {
        zwp_linux_dmabuf_v1_send_modifier(resource, TRIERARCH_DRM_FORMAT_XRGB8888, 0, 0);
        zwp_linux_dmabuf_v1_send_modifier(resource, TRIERARCH_DRM_FORMAT_ARGB8888, 0, 0);
        zwp_linux_dmabuf_v1_send_modifier(resource, TRIERARCH_DRM_FORMAT_XBGR8888, 0, 0);
        zwp_linux_dmabuf_v1_send_modifier(resource, TRIERARCH_DRM_FORMAT_ABGR8888, 0, 0);
    }
    LOGI("linux-dmabuf bound at negotiated version %u", version);
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
    trierarch_explicit_sync_buffer_release(buffer);
}
