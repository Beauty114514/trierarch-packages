#include "surface_lifecycle.h"

#include "server_internal.h"

#include <android/log.h>

#define TRIERARCH_TAG "TrierarchWayland"

static const char *surface_source_name(enum trierarch_surface_source source) {
    switch (source) {
    case TRIERARCH_SURFACE_DMABUF:
        return "dma-buf";
    case TRIERARCH_SURFACE_ANDROID_WLEGL:
        return "android-wlegl";
    case TRIERARCH_SURFACE_EGL_WL:
        return "egl-wl";
    case TRIERARCH_SURFACE_SHM:
        return "shm";
    case TRIERARCH_SURFACE_DETACHED:
        return "detached";
    }
    return "unknown";
}

enum trierarch_surface_source trierarch_surface_buffer_source(
        const struct shm_buffer *buffer) {
    if (!buffer)
        return TRIERARCH_SURFACE_DETACHED;
    if (buffer->dmabuf)
        return TRIERARCH_SURFACE_DMABUF;
    if (buffer->android_buffer)
        return TRIERARCH_SURFACE_ANDROID_WLEGL;
    if (buffer->egl_buffer)
        return TRIERARCH_SURFACE_EGL_WL;
    return TRIERARCH_SURFACE_SHM;
}

bool trierarch_surface_source_changed(enum trierarch_surface_source previous,
        enum trierarch_surface_source next) {
    return previous != next;
}

void trierarch_surface_log_lifecycle_transition(
        const struct compositor_surface *surface,
        enum trierarch_surface_source previous,
        enum trierarch_surface_source next) {
    if (!surface || !trierarch_surface_source_changed(previous, next))
        return;
    __android_log_print(ANDROID_LOG_INFO, TRIERARCH_TAG,
            "surface lifecycle: id=%u %s -> %s mapped=%d",
            wl_resource_get_id(surface->wl_surface), surface_source_name(previous),
            surface_source_name(next), surface->mapped);
}
