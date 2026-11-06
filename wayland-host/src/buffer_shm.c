#include "server_internal.h"

#include <android/log.h>
#include <pthread.h>
#include <errno.h>
#include <android/hardware_buffer.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#define TRIERARCH_TAG "TrierarchWayland"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TRIERARCH_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TRIERARCH_TAG, __VA_ARGS__)

struct shm_pool {
    void *data;
    size_t size;
    unsigned int references;
    int fd;
    struct wl_list buffers;
};

static void pool_unref(struct shm_pool *pool);

/* Buffers replaced by a new commit keep the reference the surface held until
 * the renderer has completed every frame that could still have sampled them.
 * Entries start on a "pending" list and are moved to a "ready" list at that
 * safe point; the release timer then returns them to the client at the output
 * cadence.  A separate entry per defer (rather than a pointer inside
 * shm_buffer) is required: a client may attach the same wl_buffer again before
 * its release arrives, and every commit transfers another reference. */
struct deferred_release_entry {
    struct shm_buffer *buffer;
    struct deferred_release_entry *next;
    uint64_t release_at_ns;
};

static pthread_mutex_t deferred_release_lock = PTHREAD_MUTEX_INITIALIZER;
static struct deferred_release_entry *deferred_pending_head;
static struct deferred_release_entry *deferred_pending_tail;
static unsigned int deferred_pending_count;
static struct deferred_release_entry *deferred_ready_head;
static struct deferred_release_entry *deferred_ready_tail;
static unsigned int deferred_ready_count;
static uint64_t deferred_last_release_ns;

static void release_entry(struct deferred_release_entry *entry) {
    struct shm_buffer *buffer = entry->buffer;
    free(entry);
    if (!buffer) return;
    if (buffer->deferred_refs > 0) buffer->deferred_refs--;
    if (buffer->server) buffer->server->diag_deferred_flushed++;
    trierarch_shm_buffer_release(buffer);
    trierarch_buffer_unref(buffer);
}

unsigned int trierarch_deferred_release_count(void) {
    pthread_mutex_lock(&deferred_release_lock);
    const unsigned int count = deferred_pending_count;
    pthread_mutex_unlock(&deferred_release_lock);
    return count;
}

void trierarch_defer_buffer_release(struct shm_buffer *buffer) {
    if (!buffer) return;
    struct deferred_release_entry *entry = calloc(1, sizeof(*entry));
    if (!entry) {
        /* Without an entry the surface's reference has nowhere to go.  This
         * is the OOM path; return the buffer rather than pinning it forever. */
        if (buffer->server) buffer->server->diag_deferred_overflow++;
        LOGW("deferred release allocation failed; releasing buffer=%p now",
                (void *)buffer);
        trierarch_shm_buffer_release(buffer);
        trierarch_buffer_unref(buffer);
        return;
    }
    entry->buffer = buffer;
    pthread_mutex_lock(&deferred_release_lock);
    if (deferred_pending_tail) deferred_pending_tail->next = entry;
    else deferred_pending_head = entry;
    deferred_pending_tail = entry;
    deferred_pending_count++;
    const unsigned int depth = deferred_pending_count + deferred_ready_count;
    buffer->deferred_refs++;
    pthread_mutex_unlock(&deferred_release_lock);

    if (buffer->server) {
        buffer->server->diag_deferred_enqueued++;
        if (buffer->deferred_refs > 1)
            buffer->server->diag_deferred_duplicates++;
        if (buffer->server->diag_deferred_depth_max < depth)
            buffer->server->diag_deferred_depth_max = depth;
        if (depth > TRIERARCH_MAX_DEFERRED_RELEASES) {
            buffer->server->diag_deferred_overflow++;
            if (buffer->server->diag_deferred_overflow <= 8)
                LOGW("deferred release backlog depth=%u; dropping pacing", depth);
        }
        if (buffer->server->diag_deferred_enqueued <= 8)
            LOGI("deferred release enqueue buffer=%p %dx%d refs=%u busy=%u depth=%u",
                    (void *)buffer, buffer->width, buffer->height,
                    buffer->references, buffer->busy_users, depth);
    }
}

uint64_t trierarch_mark_deferred_releases_ready(uint64_t now_ns,
        uint64_t interval_ns) {
    pthread_mutex_lock(&deferred_release_lock);
    if (!deferred_pending_head) {
        const uint64_t next = deferred_ready_head
                ? deferred_ready_head->release_at_ns : 0;
        pthread_mutex_unlock(&deferred_release_lock);
        return next;
    }
    uint64_t release_at = now_ns;
    if (interval_ns && release_at < deferred_last_release_ns + interval_ns)
        release_at = deferred_last_release_ns + interval_ns;
    deferred_last_release_ns = release_at;
    for (struct deferred_release_entry *entry = deferred_pending_head; entry;
            entry = entry->next)
        entry->release_at_ns = release_at;
    if (deferred_ready_tail) deferred_ready_tail->next = deferred_pending_head;
    else deferred_ready_head = deferred_pending_head;
    deferred_ready_tail = deferred_pending_tail;
    deferred_ready_count += deferred_pending_count;
    deferred_pending_head = deferred_pending_tail = NULL;
    deferred_pending_count = 0;
    const uint64_t next = deferred_ready_head->release_at_ns;
    pthread_mutex_unlock(&deferred_release_lock);
    return next;
}

uint64_t trierarch_release_ready_releases(uint64_t now_ns) {
    pthread_mutex_lock(&deferred_release_lock);
    struct deferred_release_entry *due = NULL;
    struct deferred_release_entry **due_tail = &due;
    while (deferred_ready_head && deferred_ready_head->release_at_ns <= now_ns) {
        struct deferred_release_entry *entry = deferred_ready_head;
        deferred_ready_head = entry->next;
        if (!deferred_ready_head) deferred_ready_tail = NULL;
        deferred_ready_count--;
        entry->next = NULL;
        *due_tail = entry;
        due_tail = &entry->next;
    }
    const uint64_t next = deferred_ready_head
            ? deferred_ready_head->release_at_ns : 0;
    pthread_mutex_unlock(&deferred_release_lock);

    while (due) {
        struct deferred_release_entry *next_entry = due->next;
        release_entry(due);
        due = next_entry;
    }
    return next;
}

void trierarch_flush_deferred_releases(void) {
    pthread_mutex_lock(&deferred_release_lock);
    struct deferred_release_entry *pending = deferred_pending_head;
    struct deferred_release_entry *ready = deferred_ready_head;
    deferred_pending_head = deferred_pending_tail = NULL;
    deferred_ready_head = deferred_ready_tail = NULL;
    deferred_pending_count = 0;
    deferred_ready_count = 0;
    deferred_last_release_ns = 0;
    pthread_mutex_unlock(&deferred_release_lock);

    while (pending) {
        struct deferred_release_entry *next = pending->next;
        release_entry(pending);
        pending = next;
    }
    while (ready) {
        struct deferred_release_entry *next = ready->next;
        release_entry(ready);
        ready = next;
    }
}

void trierarch_drain_deferred_releases(void) {
    trierarch_flush_deferred_releases();
}

void trierarch_buffer_ref(struct shm_buffer *buffer) {
    if (buffer)
        buffer->references++;
}

void trierarch_buffer_unref(struct shm_buffer *buffer) {
    if (!buffer || buffer->references == 0)
        return;
    buffer->references--;
    if (buffer->references == 0) {
        if (buffer->destroy)
            buffer->destroy(buffer);
        else
            free(buffer);
    }
}

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

void trierarch_buffer_destroy_storage(struct shm_buffer *buffer) {
    if (!buffer) return;
    /* A pending buffer release is owned by this buffer; drop it before the
     * buffer memory goes away so its destroy hook does not touch freed state. */
    if (buffer->explicit_release) {
        struct wl_resource *release = buffer->explicit_release;
        buffer->explicit_release = NULL;
        wl_resource_destroy(release);
    }
    if (buffer->egl_cache_linked) {
        wl_list_remove(&buffer->egl_cache_link);
        buffer->egl_cache_linked = false;
    }
    if (buffer->egl_buffer && buffer->egl_resource_destroy_listener_attached)
        wl_list_remove(&buffer->egl_resource_destroy_listener.link);
    if (buffer->android_hardware_buffer)
        AHardwareBuffer_release(buffer->android_hardware_buffer);
    if (buffer->dmabuf && buffer->dmabuf_mapping)
        munmap(buffer->dmabuf_mapping, buffer->dmabuf_mapping_size);
    if (buffer->dmabuf && buffer->dmabuf_fd >= 0)
        close(buffer->dmabuf_fd);
    if (buffer->tvg_token && buffer->tvg_fd >= 0)
        close(buffer->tvg_fd);
    if (buffer->pool) wl_list_remove(&buffer->pool_link);
    if (buffer->pool) pool_unref(buffer->pool);
    if (buffer->single_pixel) free(buffer->data);
    free(buffer);
}

static void buffer_resource_destroy(struct wl_resource *resource) {
    struct shm_buffer *buffer = wl_resource_get_user_data(resource);
    if (!buffer) return;
    buffer->resource = NULL;
    trierarch_buffer_unref(buffer);
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
    buffer->references = 1;
    buffer->destroy = trierarch_buffer_destroy_storage;
    buffer->dmabuf_fd = -1;
    buffer->acquire_fence_fd = -1;
    wl_list_init(&buffer->pool_link);
    pool->references++;
    wl_list_insert(&pool->buffers, &buffer->pool_link);

    buffer->resource = wl_resource_create(client, &wl_buffer_interface,
            1, id);
    if (!buffer->resource) {
        /* The buffer is already linked into pool->buffers; removing the link
         * before freeing keeps later pool_resize()/pool_unref() walks from
         * dereferencing a freed node. */
        wl_list_remove(&buffer->pool_link);
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
    /* wl_shm_pool.resize only changes the server's mapping.  The client owns
     * the backing fd and must have grown it before sending this request. */
    if (!pool || size <= 0 || (size_t)size <= pool->size) {
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
    if (!buffer || buffer->busy_users == 0) return;
    /* A wl_buffer may be attached to several surfaces at once; release only
     * when the last surface stops using it. */
    if (--buffer->busy_users > 0) return;
    if (buffer->resource)
        wl_buffer_send_release(buffer->resource);
    trierarch_explicit_sync_buffer_release(buffer);
}
