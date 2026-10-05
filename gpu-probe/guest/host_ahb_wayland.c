#define _POSIX_C_SOURCE 200809L

#include "host_ahb_channel.h"

#include <wayland-client.h>

#include "linux-dmabuf-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    bool configured;
    bool frame_done;
    bool buffer_released;
    bool detached;
    bool closed;
};

static void registry_global(void *data, struct wl_registry *registry,
        uint32_t name, const char *interface, uint32_t version) {
    struct probe *probe = data;
    if (!strcmp(interface, wl_compositor_interface.name))
        probe->compositor = wl_registry_bind(registry, name, &wl_compositor_interface,
                version < 4 ? version : 4);
    else if (!strcmp(interface, zwp_linux_dmabuf_v1_interface.name))
        probe->dmabuf = wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface,
                version < 3 ? version : 3);
    else if (!strcmp(interface, xdg_wm_base_interface.name))
        probe->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface,
                version < 6 ? version : 6);
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
};

static void wm_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };

static void surface_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
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
    (void)buffer;
    ((struct probe *)data)->buffer_released = true;
    puts("host-ahb-wayland: compositor released the Android buffer");
}

static const struct wl_buffer_listener buffer_listener = { .release = buffer_release };

static void frame_done(void *data, struct wl_callback *callback, uint32_t time) {
    (void)time;
    struct probe *probe = data;
    probe->frame_done = true;
    probe->frame = NULL;
    wl_callback_destroy(callback);
    puts("host-ahb-wayland: frame callback received");
}

static const struct wl_callback_listener frame_listener = { .done = frame_done };

static uint64_t monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
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

static int submit_host_buffer(const char *wayland_display,
        const struct trierarch_gpu_probe_buffer *host_buffer, int pixel_fd) {
    struct probe probe = {0};
    struct wl_registry *registry = NULL;
    struct zwp_linux_buffer_params_v1 *params = NULL;
    int result = EXIT_FAILURE;

    probe.display = wl_display_connect(wayland_display);
    if (!probe.display) {
        perror("host-ahb-wayland: connect parent Wayland display");
        goto out;
    }
    registry = wl_display_get_registry(probe.display);
    if (!registry || wl_registry_add_listener(registry, &registry_listener, &probe) ||
            wl_display_roundtrip(probe.display) < 0 || !probe.compositor ||
            !probe.dmabuf || !probe.wm_base) {
        fputs("host-ahb-wayland: required parent Wayland globals unavailable\n", stderr);
        goto out;
    }
    xdg_wm_base_add_listener(probe.wm_base, &wm_listener, &probe);
    probe.surface = wl_compositor_create_surface(probe.compositor);
    probe.xdg_surface = probe.surface
            ? xdg_wm_base_get_xdg_surface(probe.wm_base, probe.surface) : NULL;
    probe.toplevel = probe.xdg_surface
            ? xdg_surface_get_toplevel(probe.xdg_surface) : NULL;
    if (!probe.toplevel || xdg_surface_add_listener(probe.xdg_surface, &surface_listener, &probe) ||
            xdg_toplevel_add_listener(probe.toplevel, &toplevel_listener, &probe)) {
        fputs("host-ahb-wayland: xdg-shell setup failed\n", stderr);
        goto out;
    }
    xdg_toplevel_set_title(probe.toplevel, "Trierarch host AHB Wayland probe");
    wl_surface_commit(probe.surface);
    if (wl_display_roundtrip(probe.display) < 0 || !probe.configured) {
        fputs("host-ahb-wayland: xdg-shell configure failed\n", stderr);
        goto out;
    }
    params = zwp_linux_dmabuf_v1_create_params(probe.dmabuf);
    if (!params) goto out;
    zwp_linux_buffer_params_v1_add(params, pixel_fd, 0, 0, host_buffer->stride,
            (uint32_t)(host_buffer->modifier >> 32), (uint32_t)host_buffer->modifier);
    pixel_fd = -1; /* Wayland owns the FD after add(). */
    probe.buffer = zwp_linux_buffer_params_v1_create_immed(params,
            (int32_t)host_buffer->width, (int32_t)host_buffer->height,
            host_buffer->drm_format, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    params = NULL;
    if (!probe.buffer || wl_buffer_add_listener(probe.buffer, &buffer_listener, &probe))
        goto out;
    probe.frame = wl_surface_frame(probe.surface);
    if (!probe.frame || wl_callback_add_listener(probe.frame, &frame_listener, &probe))
        goto out;
    wl_surface_attach(probe.surface, probe.buffer, 0, 0);
    wl_surface_damage_buffer(probe.surface, 0, 0,
            (int32_t)host_buffer->width, (int32_t)host_buffer->height);
    wl_surface_commit(probe.surface);
    if (wl_display_flush(probe.display) < 0 && errno != EAGAIN) goto out;

    uint64_t deadline = monotonic_ms() + 5000;
    while (!probe.closed && monotonic_ms() < deadline &&
            (!probe.frame_done || !probe.buffer_released)) {
        if (wl_display_dispatch_pending(probe.display) < 0) goto out;
        if (probe.frame_done && !probe.detached) {
            /* The compositor cannot release a buffer that remains attached.
             * A NULL commit makes the test exercise the normal retirement
             * path instead of relying on process teardown. */
            wl_surface_attach(probe.surface, NULL, 0, 0);
            wl_surface_commit(probe.surface);
            probe.detached = true;
        }
        int flushed = wl_display_flush(probe.display);
        if (flushed < 0 && errno != EAGAIN) goto out;
        struct pollfd descriptor = {
            .fd = wl_display_get_fd(probe.display),
            .events = POLLIN | (flushed < 0 ? POLLOUT : 0),
        };
        if (poll(&descriptor, 1, 200) < 0 && errno != EINTR) goto out;
        if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) goto out;
        if (descriptor.revents & POLLIN && wl_display_dispatch(probe.display) < 0) goto out;
    }
    printf("host-ahb-wayland: frame_callback=%d released=%d closed=%d\n",
            probe.frame_done, probe.buffer_released, probe.closed);
    result = probe.frame_done && probe.buffer_released && !probe.closed ? EXIT_SUCCESS : EXIT_FAILURE;

out:
    if (result != EXIT_SUCCESS && probe.display)
        fprintf(stderr, "host-ahb-wayland: failed (Wayland error=%d)\n",
                wl_display_get_error(probe.display));
    if (params) zwp_linux_buffer_params_v1_destroy(params);
    if (registry) wl_registry_destroy(registry);
    destroy_probe(&probe);
    if (pixel_fd >= 0) close(pixel_fd);
    return result;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s GPU_PROBE_SOCKET WAYLAND_DISPLAY\n", argv[0]);
        return EXIT_FAILURE;
    }
    int probe_fd = trierarch_host_ahb_connect(argv[1]);
    if (probe_fd < 0 || trierarch_host_ahb_request_buffers(probe_fd) < 0) {
        perror("host-ahb-wayland: request Android buffers");
        return EXIT_FAILURE;
    }
    struct trierarch_gpu_probe_buffer buffer = {0};
    int pixel_fd = trierarch_host_ahb_receive_buffer(probe_fd, &buffer);
    if (pixel_fd < 0) {
        fputs("host-ahb-wayland: did not receive Android buffer\n", stderr);
        close(probe_fd);
        return EXIT_FAILURE;
    }
    printf("host-ahb-wayland: received Android buffer %ux%u format=0x%x stride=%u\n",
            buffer.width, buffer.height, buffer.drm_format, buffer.stride);
    for (unsigned int index = 1; index < 3; ++index) {
        struct trierarch_gpu_probe_buffer ignored = {0};
        int ignored_fd = trierarch_host_ahb_receive_buffer(probe_fd, &ignored);
        if (ignored_fd < 0) {
            close(pixel_fd);
            close(probe_fd);
            return EXIT_FAILURE;
        }
        close(ignored_fd);
    }
    int result = submit_host_buffer(argv[2], &buffer, pixel_fd);
    close(probe_fd);
    return result;
}
