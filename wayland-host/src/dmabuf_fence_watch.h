#ifndef TRIERARCH_DMABUF_FENCE_WATCH_H
#define TRIERARCH_DMABUF_FENCE_WATCH_H

#include <stdbool.h>
#include <stdint.h>

struct wl_event_loop;
struct wl_event_source;

typedef void (*trierarch_dmabuf_fence_ready_fn)(void *data, uint32_t mask);

struct trierarch_dmabuf_fence_watch {
    struct wl_event_source *source;
    int fd;
    trierarch_dmabuf_fence_ready_fn ready;
    void *data;
};

void trierarch_dmabuf_fence_watch_init(struct trierarch_dmabuf_fence_watch *watch);
/* Takes ownership of fd, including when registration fails. */
bool trierarch_dmabuf_fence_watch_arm(struct trierarch_dmabuf_fence_watch *watch,
        struct wl_event_loop *loop, int fd, trierarch_dmabuf_fence_ready_fn ready,
        void *data);
void trierarch_dmabuf_fence_watch_cancel(struct trierarch_dmabuf_fence_watch *watch);
bool trierarch_dmabuf_fence_watch_is_armed(const struct trierarch_dmabuf_fence_watch *watch);

#endif
