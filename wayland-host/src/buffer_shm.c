#include "server_internal.h"

#include <errno.h>
#include <android/hardware_buffer.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

struct shm_pool {
    void *data;
    size_t size;
    unsigned int references;
    int fd;
    struct wl_list buffers;
};

static void pool_unref(struct shm_pool *pool);

static void pool_unref(struct shm_pool *pool) {
    if (!pool || --pool->references != 0) return;
    munmap(pool->data, pool->size);
    close(pool->fd);
    free(pool);
}

static void buffer_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct wl_buffer_interface buffer_impl = {
    .destroy = buffer_destroy,
};

static void buffer_resource_destroy(struct wl_resource *resource) {
    struct shm_buffer *buffer = wl_resource_get_user_data(resource);
    if (!buffer) return;
    buffer->resource = NULL;
    if (buffer->android_buffer) {
        if (buffer->android_hardware_buffer)
            AHardwareBuffer_release(buffer->android_hardware_buffer);
        free(buffer);
        return;
    }
    if (buffer->dmabuf) {
        if (buffer->dmabuf_mapping)
            munmap(buffer->dmabuf_mapping, buffer->dmabuf_mapping_size);
        if (buffer->dmabuf_fd >= 0) close(buffer->dmabuf_fd);
        free(buffer);
        return;
    }
    if (buffer->pool) wl_list_remove(&buffer->pool_link);
    if (buffer->pool) pool_unref(buffer->pool);
    if (buffer->single_pixel) free(buffer->data);
    free(buffer);
}

static void pool_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void pool_resource_destroy(struct wl_resource *resource) {
    struct shm_pool *pool = wl_resource_get_user_data(resource);
    pool_unref(pool);
}

static void pool_create_buffer(struct wl_client *client, struct wl_resource *resource,
        uint32_t id, int32_t offset, int32_t width, int32_t height,
        int32_t stride, uint32_t format) {
    struct shm_pool *pool = wl_resource_get_user_data(resource);
    if (!pool || width <= 0 || height <= 0 || stride < width * 4 || offset < 0 ||
            (size_t)offset + (size_t)stride * (size_t)height > pool->size) {
        wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_STRIDE,
                "invalid wl_shm buffer dimensions");
        return;
    }
    if (format != WL_SHM_FORMAT_XRGB8888 && format != WL_SHM_FORMAT_ARGB8888) {
        wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_FORMAT,
                "only XRGB8888 and ARGB8888 are supported");
        return;
    }

    struct shm_buffer *buffer = calloc(1, sizeof(*buffer));
    if (!buffer) {
        wl_client_post_no_memory(client);
        return;
    }
    buffer->data = (char *)pool->data + offset;
    buffer->pool = pool;
    buffer->size = (size_t)stride * (size_t)height;
    buffer->offset = offset;
    buffer->width = width;
    buffer->height = height;
    buffer->stride = stride;
    buffer->format = format;
    buffer->single_pixel = false;
    buffer->dmabuf_fd = -1;
    wl_list_init(&buffer->pool_link);
    pool->references++;
    wl_list_insert(&pool->buffers, &buffer->pool_link);

    buffer->resource = wl_resource_create(client, &wl_buffer_interface,
            1, id);
    if (!buffer->resource) {
        pool_unref(pool);
        free(buffer);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(buffer->resource, &buffer_impl, buffer,
            buffer_resource_destroy);
}

static void pool_resize(struct wl_client *client, struct wl_resource *resource,
        int32_t size) {
    (void)client;
    struct shm_pool *pool = wl_resource_get_user_data(resource);
    if (!pool || size <= 0 || ftruncate(pool->fd, size) < 0) {
        wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_FD,
                "invalid wl_shm pool resize");
        return;
    }
    void *mapped = mremap(pool->data, pool->size, (size_t)size, MREMAP_MAYMOVE);
    if (mapped == MAP_FAILED) {
        wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_FD,
                "wl_shm pool remap failed");
        return;
    }
    pool->data = mapped;
    pool->size = (size_t)size;
    struct shm_buffer *buffer;
    wl_list_for_each(buffer, &pool->buffers, pool_link) {
        buffer->data = (char *)pool->data + buffer->offset;
    }
}

static const struct wl_shm_pool_interface pool_impl = {
    .create_buffer = pool_create_buffer,
    .destroy = pool_destroy,
    .resize = pool_resize,
};

static void shm_create_pool(struct wl_client *client, struct wl_resource *resource,
        uint32_t id, int fd, int32_t size) {
    if (size <= 0) {
        close(fd);
        wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_FD, "invalid pool size");
        return;
    }
    struct shm_pool *pool = calloc(1, sizeof(*pool));
    if (!pool) {
        close(fd);
        wl_client_post_no_memory(client);
        return;
    }
    pool->data = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE,
            MAP_SHARED, fd, 0);
    if (pool->data == MAP_FAILED) {
        close(fd);
        free(pool);
        wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_FD, "mmap failed");
        return;
    }
    pool->size = (size_t)size;
    pool->references = 1;
    pool->fd = fd;
    wl_list_init(&pool->buffers);

    struct wl_resource *pool_resource = wl_resource_create(
            client, &wl_shm_pool_interface,
            wl_resource_get_version(resource), id);
    if (!pool_resource) {
        pool_unref(pool);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(pool_resource, &pool_impl, pool,
            pool_resource_destroy);
}

static const struct wl_shm_interface shm_impl = {
    .create_pool = shm_create_pool,
};

void trierarch_shm_bind(struct wl_client *client, void *data,
        uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *resource = wl_resource_create(
            client, &wl_shm_interface, version < 1 ? version : 1, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &shm_impl, NULL, NULL);
    wl_shm_send_format(resource, WL_SHM_FORMAT_XRGB8888);
    wl_shm_send_format(resource, WL_SHM_FORMAT_ARGB8888);
}

struct shm_buffer *trierarch_shm_buffer_from_resource(struct wl_resource *resource) {
    if (!resource)
        return NULL;
    struct shm_buffer *buffer = wl_resource_get_user_data(resource);
    // Every wl_buffer resource currently exposed by this minimal compositor
    // is created by pool_create_buffer. Avoid wl_resource_instance_of here:
    // its implementation comparison is not reliable across libwayland
    // versions and caused valid wl_shm buffers from labwc to be rejected.
    return buffer && (buffer->pool || buffer->single_pixel) ? buffer : NULL;
}

void trierarch_shm_buffer_release(struct shm_buffer *buffer) {
    if (!buffer || !buffer->resource || !buffer->busy) return;
    buffer->busy = false;
    wl_buffer_send_release(buffer->resource);
}
