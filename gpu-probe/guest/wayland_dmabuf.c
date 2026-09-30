#define _POSIX_C_SOURCE 200809L

#include <gbm.h>
#include <drm_fourcc.h>
#include <wayland-client.h>

#include "linux-dmabuf-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct probe {
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct zwp_linux_dmabuf_v1 *dmabuf;
    struct xdg_wm_base *wm_base;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wl_buffer *buffer;
    struct wl_callback *frame;
    bool linear_xrgb, configured, frame_done, closed;
};

static void registry_global(void *data, struct wl_registry *registry,
        uint32_t name, const char *interface, uint32_t version) {
    struct probe *probe = data;
    if (!strcmp(interface, wl_compositor_interface.name))
        probe->compositor = wl_registry_bind(registry, name,
                &wl_compositor_interface, version < 4 ? version : 4);
    else if (!strcmp(interface, zwp_linux_dmabuf_v1_interface.name))
        probe->dmabuf = wl_registry_bind(registry, name,
                &zwp_linux_dmabuf_v1_interface, version < 4 ? version : 4);
    else if (!strcmp(interface, xdg_wm_base_interface.name))
        probe->wm_base = wl_registry_bind(registry, name,
                &xdg_wm_base_interface, version < 6 ? version : 6);
}

static void registry_remove(void *data, struct wl_registry *registry,
        uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

static void dmabuf_format(void *data, struct zwp_linux_dmabuf_v1 *dmabuf,
        uint32_t format) {
    (void)data;
    (void)dmabuf;
    (void)format;
}

static void dmabuf_modifier(void *data, struct zwp_linux_dmabuf_v1 *dmabuf,
        uint32_t format, uint32_t modifier_hi, uint32_t modifier_lo) {
    (void)dmabuf;
    struct probe *probe = data;
    if (format == DRM_FORMAT_XRGB8888 && !modifier_hi && !modifier_lo)
        probe->linear_xrgb = true;
}

static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
    .format = dmabuf_format,
    .modifier = dmabuf_modifier,
};

static void wm_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };

static void surface_configure(void *data, struct xdg_surface *surface,
        uint32_t serial) {
    struct probe *probe = data;
    xdg_surface_ack_configure(surface, serial);
    probe->configured = true;
}

static const struct xdg_surface_listener surface_listener = {
    .configure = surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
        int32_t width, int32_t height, struct wl_array *states) {
    (void)data;
    (void)toplevel;
    (void)width;
    (void)height;
    (void)states;
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    (void)toplevel;
    ((struct probe *)data)->closed = true;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

static void buffer_release(void *data, struct wl_buffer *buffer) {
    (void)data;
    (void)buffer;
    puts("wayland-dmabuf: compositor released the buffer");
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static void frame_done(void *data, struct wl_callback *callback, uint32_t time) {
    (void)time;
    struct probe *probe = data;
    probe->frame_done = true;
    probe->frame = NULL;
    wl_callback_destroy(callback);
    puts("wayland-dmabuf: frame callback received");
}

static const struct wl_callback_listener frame_listener = { .done = frame_done };

static uint64_t monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static int parse_number(const char *text, uint32_t maximum, uint32_t *result) {
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || !text[0] || *end || !value || value > maximum)
        return -1;
    *result = (uint32_t)value;
    return 0;
}

static int fill_checkerboard(struct gbm_bo *bo, uint32_t width, uint32_t height) {
    uint32_t stride = 0;
    void *map_data = NULL;
    uint32_t *pixels = gbm_bo_map(bo, 0, 0, width, height,
            GBM_BO_TRANSFER_WRITE, &stride, &map_data);
    if (!pixels)
        return -1;
    uint32_t tile = (width < height ? width : height) / 8;
    if (!tile)
        tile = 1;
    for (uint32_t y = 0; y < height; ++y) {
        uint32_t *row = (uint32_t *)((uint8_t *)pixels + y * stride);
        for (uint32_t x = 0; x < width; ++x)
            row[x] = ((x / tile) + (y / tile)) & 1 ? 0x00ffffffu : 0;
    }
    gbm_bo_unmap(bo, map_data);
    return 0;
}

static void destroy_probe(struct probe *probe) {
    if (probe->frame) wl_callback_destroy(probe->frame);
    if (probe->buffer) wl_buffer_destroy(probe->buffer);
    if (probe->toplevel) xdg_toplevel_destroy(probe->toplevel);
    if (probe->xdg_surface) xdg_surface_destroy(probe->xdg_surface);
    if (probe->surface) wl_surface_destroy(probe->surface);
    if (probe->wm_base) xdg_wm_base_destroy(probe->wm_base);
    if (probe->dmabuf) zwp_linux_dmabuf_v1_destroy(probe->dmabuf);
    if (probe->compositor) wl_compositor_destroy(probe->compositor);
    if (probe->display) wl_display_disconnect(probe->display);
}

int main(int argc, char **argv) {
    if (argc < 4 || argc > 5) {
        fprintf(stderr, "usage: %s WAYLAND_DISPLAY width height [hold-seconds]\n", argv[0]);
        return EXIT_FAILURE;
    }
    uint32_t width, height, hold_seconds = 5;
    if (parse_number(argv[2], 4096, &width) ||
            parse_number(argv[3], 4096, &height) ||
            (argc == 5 && parse_number(argv[4], 30, &hold_seconds))) {
        fputs("wayland-dmabuf: width/height must be 1..4096; hold-seconds 1..30\n", stderr);
        return EXIT_FAILURE;
    }

    int result = EXIT_FAILURE;
    int render_fd = -1, pixel_fd = -1;
    struct gbm_device *device = NULL;
    struct gbm_bo *bo = NULL;
    struct probe probe = {0};
    struct wl_registry *registry = NULL;
    struct zwp_linux_buffer_params_v1 *params = NULL;

    probe.display = wl_display_connect(argv[1]);
    if (!probe.display) {
        perror("wayland-dmabuf: connect");
        goto out;
    }
    registry = wl_display_get_registry(probe.display);
    if (!registry || wl_registry_add_listener(registry, &registry_listener, &probe) ||
            wl_display_roundtrip(probe.display) < 0 ||
            !probe.compositor || !probe.dmabuf || !probe.wm_base) {
        fputs("wayland-dmabuf: required Wayland globals unavailable\n", stderr);
        goto out;
    }
    zwp_linux_dmabuf_v1_add_listener(probe.dmabuf, &dmabuf_listener, &probe);
    xdg_wm_base_add_listener(probe.wm_base, &wm_listener, &probe);
    if (wl_display_roundtrip(probe.display) < 0 || !probe.linear_xrgb) {
        fputs("wayland-dmabuf: linear XRGB8888 is not advertised\n", stderr);
        goto out;
    }

    probe.surface = wl_compositor_create_surface(probe.compositor);
    probe.xdg_surface = probe.surface
            ? xdg_wm_base_get_xdg_surface(probe.wm_base, probe.surface) : NULL;
    probe.toplevel = probe.xdg_surface
            ? xdg_surface_get_toplevel(probe.xdg_surface) : NULL;
    if (!probe.toplevel ||
            xdg_surface_add_listener(probe.xdg_surface, &surface_listener, &probe) ||
            xdg_toplevel_add_listener(probe.toplevel, &toplevel_listener, &probe)) {
        fputs("wayland-dmabuf: xdg-shell role failed\n", stderr);
        goto out;
    }
    wl_surface_commit(probe.surface);
    if (wl_display_roundtrip(probe.display) < 0 || !probe.configured) {
        fputs("wayland-dmabuf: xdg-shell configure failed\n", stderr);
        goto out;
    }
    xdg_toplevel_set_title(probe.toplevel, "Trierarch DMA-BUF probe");

    render_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    device = render_fd >= 0 ? gbm_create_device(render_fd) : NULL;
    bo = device ? gbm_bo_create(device, width, height, GBM_FORMAT_XRGB8888,
            GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR) : NULL;
    pixel_fd = bo && fill_checkerboard(bo, width, height) == 0 ? gbm_bo_get_fd(bo) : -1;
    if (pixel_fd < 0) {
        fputs("wayland-dmabuf: GBM allocation/map/export failed\n", stderr);
        goto out;
    }
    uint32_t stride = gbm_bo_get_stride(bo);
    uint64_t modifier = gbm_bo_get_modifier(bo);
    if (modifier != 0 || stride < width * 4) {
        fprintf(stderr, "wayland-dmabuf: expected linear XRGB buffer, got modifier=0x%llx stride=%u\n",
                (unsigned long long)modifier, stride);
        goto out;
    }
    struct stat status = {0};
    if (fstat(pixel_fd, &status) < 0 || status.st_size <= 0) {
        perror("wayland-dmabuf: fstat");
        goto out;
    }
    printf("wayland-dmabuf: guest %ux%u fmt=0x%x stride=%u modifier=0x%llx fd=%lld\n",
            width, height, DRM_FORMAT_XRGB8888, stride,
            (unsigned long long)modifier, (long long)status.st_size);
    fflush(stdout);

    params = zwp_linux_dmabuf_v1_create_params(probe.dmabuf);
    if (!params) goto out;
    zwp_linux_buffer_params_v1_add(params, pixel_fd, 0, 0, stride,
            (uint32_t)(modifier >> 32), (uint32_t)modifier);
    probe.buffer = zwp_linux_buffer_params_v1_create_immed(params,
            (int32_t)width, (int32_t)height, DRM_FORMAT_XRGB8888, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    params = NULL;
    if (!probe.buffer || wl_buffer_add_listener(probe.buffer, &buffer_listener, &probe))
        goto out;

    probe.frame = wl_surface_frame(probe.surface);
    if (!probe.frame || wl_callback_add_listener(probe.frame, &frame_listener, &probe))
        goto out;
    wl_surface_attach(probe.surface, probe.buffer, 0, 0);
    wl_surface_damage(probe.surface, 0, 0, (int32_t)width, (int32_t)height);
    wl_surface_commit(probe.surface);
    if (wl_display_flush(probe.display) < 0 && errno != EAGAIN)
        goto out;
    close(pixel_fd);
    pixel_fd = -1;

    uint64_t deadline = monotonic_ms() + (uint64_t)hold_seconds * 1000;
    while (!probe.closed && monotonic_ms() < deadline) {
        if (wl_display_dispatch_pending(probe.display) < 0)
            goto out;
        int flushed = wl_display_flush(probe.display);
        if (flushed < 0 && errno != EAGAIN)
            goto out;
        struct pollfd descriptor = {
            .fd = wl_display_get_fd(probe.display),
            .events = POLLIN | (flushed < 0 ? POLLOUT : 0),
        };
        int wait = poll(&descriptor, 1, 200);
        if (wait < 0 && errno == EINTR)
            continue;
        if (wait < 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)))
            goto out;
        if (descriptor.revents & POLLIN && wl_display_dispatch(probe.display) < 0)
            goto out;
    }
    printf("wayland-dmabuf: frame_callback=%d closed=%d\n",
            probe.frame_done, probe.closed);
    result = probe.frame_done && !probe.closed ? EXIT_SUCCESS : EXIT_FAILURE;

out:
    if (result != EXIT_SUCCESS && probe.display)
        fprintf(stderr, "wayland-dmabuf: failed (Wayland error=%d)\n",
                wl_display_get_error(probe.display));
    if (params) zwp_linux_buffer_params_v1_destroy(params);
    if (registry) wl_registry_destroy(registry);
    destroy_probe(&probe);
    if (pixel_fd >= 0) close(pixel_fd);
    if (bo) gbm_bo_destroy(bo);
    if (device) gbm_device_destroy(device);
    if (render_fd >= 0) close(render_fd);
    return result;
}
