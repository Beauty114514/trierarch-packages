#include "android_presenter_probe.h"

#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

#define TAG "TrierarchPresenter"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

/*
 * These definitions describe the stable Android native-window producer ABI
 * needed for dequeueBuffer(). They are intentionally kept local to this
 * probe: neither the presentation path nor a guest receives these internals.
 */
typedef struct native_handle {
    int version;
    int numFds;
    int numInts;
    int data[];
} native_handle_t;

typedef struct android_native_base {
    int magic;
    int version;
    void *reserved[4];
    void (*inc_ref)(struct android_native_base *base);
    void (*dec_ref)(struct android_native_base *base);
} android_native_base_t;

typedef struct presenter_native_window_buffer {
    android_native_base_t common;
    int width;
    int height;
    int stride;
    int format;
    int usage_deprecated;
    uintptr_t layer_count;
    void *reserved[1];
    const native_handle_t *handle;
} presenter_native_window_buffer_t;

typedef int (*dequeue_buffer_fn)(ANativeWindow *, presenter_native_window_buffer_t **,
        int *acquire_fence_fd);
typedef int (*cancel_buffer_fn)(ANativeWindow *, presenter_native_window_buffer_t *,
        int release_fence_fd);

enum {
    ANW_API_CONNECT = 13,
    ANW_API_DISCONNECT = 14,
    ANW_API_CPU = 2,
};

struct presenter_native_window {
    android_native_base_t common;
    const uint32_t flags;
    const int min_swap_interval;
    const int max_swap_interval;
    const float xdpi;
    const float ydpi;
    intptr_t oem[4];
    int (*set_swap_interval)(struct presenter_native_window *, int);
    void *dequeue_buffer_deprecated;
    void *lock_buffer_deprecated;
    void *queue_buffer_deprecated;
    int (*query)(const struct presenter_native_window *, int, int *);
    int (*perform)(struct presenter_native_window *, int, ...);
};

static int window_api(struct presenter_native_window *window, int operation, int api) {
    return window->perform(window, operation, api);
}

void trierarch_android_presenter_probe(ANativeWindow *window) {
    static bool probed;
    if (!window || probed)
        return;
    probed = true;

    void *library = dlopen("libnativewindow.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        LOGW("presenter probe result=unavailable reason=libnativewindow");
        return;
    }
    dequeue_buffer_fn dequeue = (dequeue_buffer_fn)dlsym(library,
            "ANativeWindow_dequeueBuffer");
    cancel_buffer_fn cancel = (cancel_buffer_fn)dlsym(library, "ANativeWindow_cancelBuffer");
    if (!dequeue || !cancel) {
        LOGW("presenter probe result=unavailable reason=dequeue-symbol");
        dlclose(library);
        return;
    }

    struct presenter_native_window *native_window =
            (struct presenter_native_window *)window;
    if (!native_window->perform || window_api(native_window, ANW_API_CONNECT, ANW_API_CPU) != 0) {
        LOGW("presenter probe result=unavailable reason=cpu-connect");
        dlclose(library);
        return;
    }

    presenter_native_window_buffer_t *buffer = NULL;
    int acquire_fence_fd = -1;
    const int status = dequeue(window, &buffer, &acquire_fence_fd);
    if (status != 0 || !buffer) {
        LOGW("presenter probe result=unavailable reason=dequeue status=%d", status);
        if (acquire_fence_fd >= 0)
            close(acquire_fence_fd);
        (void)window_api(native_window, ANW_API_DISCONNECT, ANW_API_CPU);
        dlclose(library);
        return;
    }

    if (acquire_fence_fd >= 0)
        close(acquire_fence_fd);
    const int fd = buffer->handle && buffer->handle->numFds > 0 ? buffer->handle->data[0] : -1;
    const bool fd_valid = fd >= 0 && fcntl(fd, F_GETFD) != -1;
    LOGI("presenter probe result=%s width=%d height=%d stride=%d format=%d handle-fds=%d",
            fd_valid ? "ready" : "unavailable", buffer->width, buffer->height, buffer->stride,
            buffer->format, buffer->handle ? buffer->handle->numFds : 0);

    /* Return the slot untouched: the normal EGL renderer will own presentation. */
    const int cancel_status = cancel(window, buffer, -1);
    if (cancel_status != 0)
        LOGW("presenter probe cancel result=failed status=%d", cancel_status);
    (void)window_api(native_window, ANW_API_DISCONNECT, ANW_API_CPU);
    dlclose(library);
}
