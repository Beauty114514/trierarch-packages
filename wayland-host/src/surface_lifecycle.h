#ifndef TRIERARCH_SURFACE_LIFECYCLE_H
#define TRIERARCH_SURFACE_LIFECYCLE_H

#include <stdbool.h>

struct compositor_surface;
struct shm_buffer;

enum trierarch_surface_source {
    TRIERARCH_SURFACE_DETACHED,
    TRIERARCH_SURFACE_SHM,
    TRIERARCH_SURFACE_DMABUF,
    TRIERARCH_SURFACE_ANDROID_WLEGL,
    TRIERARCH_SURFACE_EGL_WL,
};

enum trierarch_surface_source trierarch_surface_buffer_source(
        const struct shm_buffer *buffer);
bool trierarch_surface_source_changed(enum trierarch_surface_source previous,
        enum trierarch_surface_source next);
void trierarch_surface_log_lifecycle_transition(
        const struct compositor_surface *surface,
        enum trierarch_surface_source previous,
        enum trierarch_surface_source next);

#endif
