#include "dmabuf_fence_watch.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <wayland-server-core.h>

struct callback_result {
    unsigned int calls;
    uint32_t mask;
};

static void fence_ready(void *data, uint32_t mask) {
    struct callback_result *result = data;
    result->calls++;
    result->mask = mask;
}

int main(void) {
    struct wl_event_loop *loop = wl_event_loop_create();
    assert(loop);
    int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    assert(fd >= 0);
    struct trierarch_dmabuf_fence_watch watch;
    struct callback_result result = {0};
    trierarch_dmabuf_fence_watch_init(&watch);
    assert(trierarch_dmabuf_fence_watch_arm(&watch, loop, fd, fence_ready, &result));
    assert(trierarch_dmabuf_fence_watch_is_armed(&watch));
    assert(wl_event_loop_dispatch(loop, 0) == 0);
    assert(result.calls == 0);
    const uint64_t signal = 1;
    assert(write(fd, &signal, sizeof(signal)) == (ssize_t)sizeof(signal));
    assert(wl_event_loop_dispatch(loop, -1) == 0);
    assert(result.calls == 1);
    assert(result.mask & WL_EVENT_READABLE);
    assert(!trierarch_dmabuf_fence_watch_is_armed(&watch));
    wl_event_loop_destroy(loop);
    puts("dmabuf_fence_watch_test: ok");
    return 0;
}
