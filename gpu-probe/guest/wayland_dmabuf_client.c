#include "wayland_dmabuf_client.h"
#include "vulkan_export.h"

#include "linux-dmabuf-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

struct dmabuf_client {
    struct wl_compositor *compositor;
    struct zwp_linux_dmabuf_v1 *dmabuf;
    struct xdg_wm_base *xdg_wm_base;
    uint32_t compositor_version;
};

struct submitted_frame {
    bool callback_done;
    bool buffer_released;
};

struct configured_surface {
    bool configured;
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
        const char *interface, uint32_t version) {
    struct dmabuf_client *client = data;
    if (!strcmp(interface, wl_compositor_interface.name)) {
        client->compositor_version = version < 4 ? version : 4;
        client->compositor = wl_registry_bind(registry, name, &wl_compositor_interface,
                client->compositor_version);
    } else if (!strcmp(interface, zwp_linux_dmabuf_v1_interface.name)) {
        uint32_t bind_version = version < 3 ? version : 3;
        if (bind_version >= 3)
            client->dmabuf = wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface,
                    bind_version);
    } else if (!strcmp(interface, xdg_wm_base_interface.name)) {
        client->xdg_wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
    }
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

static void dmabuf_format(void *data, struct zwp_linux_dmabuf_v1 *dmabuf, uint32_t format) {
    (void)data;
    (void)dmabuf;
    (void)format;
}

static void dmabuf_modifier(void *data, struct zwp_linux_dmabuf_v1 *dmabuf, uint32_t format,
        uint32_t modifier_hi, uint32_t modifier_lo) {
    (void)data;
    (void)dmabuf;
    (void)format;
    (void)modifier_hi;
    (void)modifier_lo;
}

static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
    .format = dmabuf_format,
    .modifier = dmabuf_modifier,
};

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = { .ping = xdg_wm_base_ping };

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial) {
    struct configured_surface *surface = data;
    xdg_surface_ack_configure(xdg_surface, serial);
    surface->configured = true;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void frame_done(void *data, struct wl_callback *callback, uint32_t time) {
    struct submitted_frame *frame = data;
    (void)time;
    frame->callback_done = true;
    wl_callback_destroy(callback);
}

static const struct wl_callback_listener frame_listener = { .done = frame_done };

static void buffer_release(void *data, struct wl_buffer *buffer) {
    struct submitted_frame *frame = data;
    (void)buffer;
    frame->buffer_released = true;
}

static const struct wl_buffer_listener buffer_listener = { .release = buffer_release };

static int dispatch_until_configured(struct wl_display *display,
        const struct configured_surface *surface) {
    const int display_fd = wl_display_get_fd(display);
    for (unsigned int seconds = 0; seconds < 5; seconds++) {
        if (wl_display_dispatch_pending(display) < 0) return -1;
        if (surface->configured) return 0;
        if (wl_display_flush(display) < 0 && errno != EAGAIN) return -1;
        struct pollfd descriptor = { .fd = display_fd, .events = POLLIN };
        int result;
        do { result = poll(&descriptor, 1, 1000); } while (result < 0 && errno == EINTR);
        if (result < 0 || (result > 0 && wl_display_dispatch(display) < 0)) return -1;
    }
    fputs("guest: xdg surface was not configured within five seconds\n", stderr);
    return -1;
}

static int dispatch_until_released(struct wl_display *display, struct submitted_frame *frame) {
    const int display_fd = wl_display_get_fd(display);
    for (unsigned int seconds = 0; seconds < 5; seconds++) {
        if (wl_display_dispatch_pending(display) < 0) return -1;
        if (wl_display_flush(display) < 0 && errno != EAGAIN) return -1;
        if (frame->callback_done && frame->buffer_released) return 0;
        struct pollfd descriptor = { .fd = display_fd, .events = POLLIN };
        int result;
        do { result = poll(&descriptor, 1, 1000); } while (result < 0 && errno == EINTR);
        if (result < 0 || (result > 0 && wl_display_dispatch(display) < 0)) return -1;
    }
    fprintf(stderr, "guest: Wayland dma-buf was not released within five seconds\n");
    return -1;
}

int trierarch_guest_submit_wayland_dmabuf(void) {
    struct trierarch_guest_vulkan_image image = {0};
    int dma_buf_fd = -1;
    uint32_t stride = 0;
    uint64_t modifier = 0;
    if (trierarch_guest_create_exportable_image(&image, &dma_buf_fd, &stride, &modifier) < 0) {
        fputs("guest: unable to create exportable Turnip dma-buf\n", stderr);
        return 1;
    }

    int status = 1;
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "guest: Wayland connection failed: %s\n", strerror(errno));
        goto out;
    }
    struct dmabuf_client client = {0};
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, &client);
    if (wl_display_roundtrip(display) < 0 || !client.compositor || !client.dmabuf ||
            !client.xdg_wm_base) {
        fputs("guest: compositor lacks required xdg-shell or linux-dmabuf v3\n", stderr);
        goto disconnect;
    }
    zwp_linux_dmabuf_v1_add_listener(client.dmabuf, &dmabuf_listener, &client);
    xdg_wm_base_add_listener(client.xdg_wm_base, &xdg_wm_base_listener, &client);
    if (wl_display_roundtrip(display) < 0) goto disconnect;

    struct wl_surface *surface = wl_compositor_create_surface(client.compositor);
    if (!surface) goto disconnect;
    struct configured_surface configured = {0};
    struct xdg_surface *xdg_surface = xdg_wm_base_get_xdg_surface(client.xdg_wm_base, surface);
    if (!xdg_surface) goto destroy_surface;
    xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, &configured);
    struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg_surface);
    if (!toplevel) goto destroy_xdg_surface;
    xdg_toplevel_set_title(toplevel, "Trierarch dma-buf probe");
    wl_surface_commit(surface);
    if (dispatch_until_configured(display, &configured) < 0) goto destroy_toplevel;

    struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(client.dmabuf);
    if (!params) goto destroy_toplevel;
    zwp_linux_buffer_params_v1_add(params, dma_buf_fd, 0, 0, stride,
            (uint32_t)(modifier >> 32), (uint32_t)modifier);
    dma_buf_fd = -1; /* Wayland now owns this FD. */
    struct wl_buffer *buffer = zwp_linux_buffer_params_v1_create_immed(params, 64, 64,
            TRIERARCH_GUEST_DRM_FORMAT_ABGR8888, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    if (!buffer) goto destroy_toplevel;

    struct submitted_frame frame = {0};
    wl_buffer_add_listener(buffer, &buffer_listener, &frame);
    struct wl_callback *callback = wl_surface_frame(surface);
    wl_callback_add_listener(callback, &frame_listener, &frame);
    wl_surface_attach(surface, buffer, 0, 0);
    if (client.compositor_version >= 4)
        wl_surface_damage_buffer(surface, 0, 0, 64, 64);
    else
        wl_surface_damage(surface, 0, 0, 64, 64);
    wl_surface_commit(surface);
    if (dispatch_until_released(display, &frame) == 0) {
        puts("guest: submitted Vulkan dma-buf through Wayland linux-dmabuf");
        status = 0;
    }
    wl_buffer_destroy(buffer);
destroy_toplevel:
    xdg_toplevel_destroy(toplevel);
destroy_xdg_surface:
    xdg_surface_destroy(xdg_surface);
destroy_surface:
    wl_surface_destroy(surface);
disconnect:
    if (client.dmabuf) zwp_linux_dmabuf_v1_destroy(client.dmabuf);
    if (client.xdg_wm_base) xdg_wm_base_destroy(client.xdg_wm_base);
    if (client.compositor) wl_compositor_destroy(client.compositor);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
out:
    if (dma_buf_fd >= 0) close(dma_buf_fd);
    trierarch_guest_destroy_exportable_image(&image);
    return status;
}
