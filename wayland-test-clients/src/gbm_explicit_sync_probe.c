#define _POSIX_C_SOURCE 200809L

#include <gbm.h>
#include <wayland-client.h>
#include <xf86drm.h>

#include "linux-dmabuf-v1-client-protocol.h"
#include "linux-explicit-synchronization-unstable-v1-client-protocol.h"

#include <drm_fourcc.h>
#include <drm.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
    BUFFER_WIDTH = 64,
    BUFFER_HEIGHT = 64,
    BYTES_PER_PIXEL = 4,
};

struct globals {
    struct wl_compositor *compositor;
    struct zwp_linux_dmabuf_v1 *dmabuf;
    struct zwp_linux_explicit_synchronization_v1 *explicit_sync;
};

struct release_result {
    unsigned int immediate;
    unsigned int fenced;
};

static void registry_global(void *data, struct wl_registry *registry,
        uint32_t name, const char *interface, uint32_t version) {
    struct globals *globals = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        globals->compositor = wl_registry_bind(registry, name,
                &wl_compositor_interface, version < 4 ? version : 4);
    } else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
        globals->dmabuf = wl_registry_bind(registry, name,
                &zwp_linux_dmabuf_v1_interface, version < 3 ? version : 3);
    } else if (strcmp(interface, zwp_linux_explicit_synchronization_v1_interface.name) == 0) {
        globals->explicit_sync = wl_registry_bind(registry, name,
                &zwp_linux_explicit_synchronization_v1_interface, 1);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
        uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void sleep_ms(long milliseconds) {
    struct timespec delay = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000000L,
    };
    nanosleep(&delay, NULL);
}

static int paint_buffer(struct gbm_bo *buffer) {
    uint32_t stride = 0;
    void *map_data = NULL;
    uint32_t *pixels = gbm_bo_map(buffer, 0, 0, BUFFER_WIDTH, BUFFER_HEIGHT,
            GBM_BO_TRANSFER_WRITE, &stride, &map_data);
    if (!pixels || stride < BUFFER_WIDTH * BYTES_PER_PIXEL)
        return -1;
    for (uint32_t y = 0; y < BUFFER_HEIGHT; y++) {
        for (uint32_t x = 0; x < BUFFER_WIDTH; x++)
            pixels[(size_t)y * stride / BYTES_PER_PIXEL + x] = 0x000000ffu;
    }
    gbm_bo_unmap(buffer, map_data);
    return 0;
}

static struct wl_buffer *create_dmabuf_buffer(struct zwp_linux_dmabuf_v1 *dmabuf,
        struct gbm_bo *buffer) {
    uint64_t modifier = gbm_bo_get_modifier(buffer);
    if (modifier != DRM_FORMAT_MOD_LINEAR && modifier != DRM_FORMAT_MOD_INVALID)
        return NULL;
    int fd = gbm_bo_get_fd(buffer);
    if (fd < 0)
        return NULL;
    struct zwp_linux_buffer_params_v1 *params =
            zwp_linux_dmabuf_v1_create_params(dmabuf);
    zwp_linux_buffer_params_v1_add(params, fd, 0, 0, gbm_bo_get_stride(buffer),
            (uint32_t)(modifier >> 32), (uint32_t)modifier);
    struct wl_buffer *result = zwp_linux_buffer_params_v1_create_immed(params,
            BUFFER_WIDTH, BUFFER_HEIGHT, DRM_FORMAT_XRGB8888, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    return result;
}

static int create_signaled_sync_file(int render_fd) {
    uint32_t handle = 0;
    int sync_file = -1;
    int result = drmSyncobjCreate(render_fd, DRM_SYNCOBJ_CREATE_SIGNALED, &handle);
    if (result != 0) {
        fprintf(stderr, "probe: drmSyncobjCreate failed: %s\n", strerror(-result));
        return -1;
    }
    result = drmSyncobjExportSyncFile(render_fd, handle, &sync_file);
    if (result != 0) {
        fprintf(stderr, "probe: drmSyncobjExportSyncFile failed: %s\n", strerror(-result));
        sync_file = -1;
    }
    drmSyncobjDestroy(render_fd, handle);
    return sync_file;
}

static void release_fenced(void *data, struct zwp_linux_buffer_release_v1 *release,
        int32_t fence) {
    struct release_result *result = data;
    result->fenced++;
    if (fence >= 0)
        close(fence);
    zwp_linux_buffer_release_v1_destroy(release);
}

static void release_immediate(void *data, struct zwp_linux_buffer_release_v1 *release) {
    struct release_result *result = data;
    result->immediate++;
    zwp_linux_buffer_release_v1_destroy(release);
}

static const struct zwp_linux_buffer_release_v1_listener release_listener = {
    .fenced_release = release_fenced,
    .immediate_release = release_immediate,
};

int main(int argc, char **argv) {
    int release_only = argc == 2 && strcmp(argv[1], "--release-only") == 0;
    if (argc != 1 && !release_only) {
        fprintf(stderr, "Usage: %s [--release-only]\n", argv[0]);
        return EXIT_FAILURE;
    }
    int status = EXIT_FAILURE;
    int render_fd = -1;
    struct gbm_device *device = NULL;
    struct gbm_bo *gbm_buffer = NULL;
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "probe: unable to connect to WAYLAND_DISPLAY\n");
        return EXIT_FAILURE;
    }
    struct globals globals = {0};
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, &globals);
    if (wl_display_roundtrip(display) < 0 || !globals.compositor || !globals.dmabuf ||
            !globals.explicit_sync) {
        fprintf(stderr, "probe: host lacks compositor, dma-buf, or explicit sync\n");
        goto out;
    }
    render_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    device = render_fd >= 0 ? gbm_create_device(render_fd) : NULL;
    gbm_buffer = device ? gbm_bo_create(device, BUFFER_WIDTH, BUFFER_HEIGHT,
            GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR) : NULL;
    if (!gbm_buffer || paint_buffer(gbm_buffer) != 0) {
        fprintf(stderr, "probe: unable to create linear GBM buffer\n");
        goto out;
    }
    struct wl_buffer *buffer = create_dmabuf_buffer(globals.dmabuf, gbm_buffer);
    struct wl_surface *surface = wl_compositor_create_surface(globals.compositor);
    struct zwp_linux_surface_synchronization_v1 *sync = surface
            ? zwp_linux_explicit_synchronization_v1_get_synchronization(
                    globals.explicit_sync, surface) : NULL;
    int acquire_fence = release_only ? -1 : create_signaled_sync_file(render_fd);
    if (!buffer || !surface || !sync || (!release_only && acquire_fence < 0)) {
        fprintf(stderr, "probe: setup failed: buffer=%s surface=%s sync=%s sync_file=%s\n",
                buffer ? "ready" : "missing", surface ? "ready" : "missing",
                sync ? "ready" : "missing", acquire_fence >= 0 ? "ready" : "missing");
        goto out_buffers;
    }
    struct zwp_linux_buffer_release_v1 *release =
            zwp_linux_surface_synchronization_v1_get_release(sync);
    if (!release) {
        fprintf(stderr, "probe: unable to request explicit-sync release object\n");
        goto out_sync;
    }
    struct release_result result = {0};
    zwp_linux_buffer_release_v1_add_listener(release, &release_listener, &result);
    if (!release_only)
        zwp_linux_surface_synchronization_v1_set_acquire_fence(sync, acquire_fence);
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, BUFFER_WIDTH, BUFFER_HEIGHT);
    wl_surface_commit(surface);
    if (wl_display_roundtrip(display) < 0) {
        fprintf(stderr, "probe: explicit-sync commit failed\n");
        goto out_sync;
    }
    printf("probe: explicit sync %s accepted\n",
            release_only ? "release-only commit" : "acquire fence");
    sleep_ms(200);
    wl_surface_attach(surface, NULL, 0, 0);
    wl_surface_commit(surface);
    if (wl_display_roundtrip(display) < 0 || (result.immediate + result.fenced) != 1) {
        fprintf(stderr, "probe: expected exactly one release, got immediate=%u fenced=%u\n",
                result.immediate, result.fenced);
        goto out_sync;
    }
    if (result.immediate != 1 || result.fenced != 0) {
        fprintf(stderr, "probe: expected immediate release, got immediate=%u fenced=%u\n",
                result.immediate, result.fenced);
        goto out_sync;
    }
    printf("probe: explicit sync immediate release received\n");
    status = EXIT_SUCCESS;

out_sync:
    zwp_linux_surface_synchronization_v1_destroy(sync);
    wl_surface_destroy(surface);
    wl_buffer_destroy(buffer);
out_buffers:
    if (gbm_buffer)
        gbm_bo_destroy(gbm_buffer);
    if (device)
        gbm_device_destroy(device);
    if (render_fd >= 0)
        close(render_fd);
out:
    if (globals.explicit_sync)
        zwp_linux_explicit_synchronization_v1_destroy(globals.explicit_sync);
    if (globals.dmabuf)
        zwp_linux_dmabuf_v1_destroy(globals.dmabuf);
    if (globals.compositor)
        wl_compositor_destroy(globals.compositor);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    if (status == EXIT_SUCCESS)
        printf("probe: complete\n");
    return status;
}
