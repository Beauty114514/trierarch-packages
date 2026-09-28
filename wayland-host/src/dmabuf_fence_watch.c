#include "dmabuf_fence_watch.h"

#include <wayland-server-core.h>

#include <unistd.h>

static int fence_ready(int fd, uint32_t mask, void *data) {
    struct trierarch_dmabuf_fence_watch *watch = data;
    if (!watch)
        return 0;
    struct wl_event_source *source = watch->source;
    trierarch_dmabuf_fence_ready_fn ready = watch->ready;
    void *ready_data = watch->data;
    watch->source = NULL;
    watch->fd = -1;
    watch->ready = NULL;
    watch->data = NULL;
    if (source)
        wl_event_source_remove(source);
    if (fd >= 0)
        close(fd);
    if (ready)
        ready(ready_data, mask);
    return 0;
}

void trierarch_dmabuf_fence_watch_init(struct trierarch_dmabuf_fence_watch *watch) {
    if (!watch)
        return;
    watch->source = NULL;
    watch->fd = -1;
    watch->ready = NULL;
    watch->data = NULL;
}

bool trierarch_dmabuf_fence_watch_arm(struct trierarch_dmabuf_fence_watch *watch,
        struct wl_event_loop *loop, int fd, trierarch_dmabuf_fence_ready_fn ready,
        void *data) {
    if (!watch || !loop || fd < 0 || !ready) {
        if (fd >= 0)
            close(fd);
        return false;
    }
    trierarch_dmabuf_fence_watch_cancel(watch);
    struct wl_event_source *source = wl_event_loop_add_fd(loop, fd,
            WL_EVENT_READABLE | WL_EVENT_ERROR | WL_EVENT_HANGUP, fence_ready, watch);
    if (!source) {
        close(fd);
        return false;
    }
    watch->source = source;
    watch->fd = fd;
    watch->ready = ready;
    watch->data = data;
    return true;
}

void trierarch_dmabuf_fence_watch_cancel(struct trierarch_dmabuf_fence_watch *watch) {
    if (!watch)
        return;
    if (watch->source)
        wl_event_source_remove(watch->source);
    if (watch->fd >= 0)
        close(watch->fd);
    trierarch_dmabuf_fence_watch_init(watch);
}

bool trierarch_dmabuf_fence_watch_is_armed(const struct trierarch_dmabuf_fence_watch *watch) {
    return watch && watch->source != NULL;
}
