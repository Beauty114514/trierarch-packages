#define _POSIX_C_SOURCE 200809L

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include "xdg-shell-client-protocol.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct probe {
    struct wl_compositor *compositor;
    struct xdg_wm_base *wm_base;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    bool configured;
    bool closed;
};

static void registry_global(void *data, struct wl_registry *registry,
        uint32_t name, const char *interface, uint32_t version) {
    struct probe *probe = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0)
        probe->compositor = wl_registry_bind(registry, name,
                &wl_compositor_interface, version < 4 ? version : 4);
    else if (strcmp(interface, xdg_wm_base_interface.name) == 0)
        probe->wm_base = wl_registry_bind(registry, name,
                &xdg_wm_base_interface, version < 6 ? version : 6);
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
};

static void wm_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
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

static void sleep_ms(long milliseconds) {
    struct timespec delay = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000000L,
    };
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

static const char *text_or_unknown(const char *value) {
    return value ? value : "unknown";
}

static void destroy_probe(struct probe *probe) {
    if (probe->toplevel)
        xdg_toplevel_destroy(probe->toplevel);
    if (probe->xdg_surface)
        xdg_surface_destroy(probe->xdg_surface);
    if (probe->surface)
        wl_surface_destroy(probe->surface);
    if (probe->wm_base)
        xdg_wm_base_destroy(probe->wm_base);
    if (probe->compositor)
        wl_compositor_destroy(probe->compositor);
}

int main(int argc, char **argv) {
    const char *socket = argc > 1 ? argv[1] : NULL;
    if (argc > 2) {
        fprintf(stderr, "usage: %s [WAYLAND_DISPLAY]\n", argv[0]);
        return EXIT_FAILURE;
    }

    int result = EXIT_FAILURE;
    struct probe probe = {0};
    struct wl_display *display = wl_display_connect(socket);
    struct wl_registry *registry = NULL;
    struct wl_egl_window *window = NULL;
    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLSurface egl_surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    EGLConfig config = NULL;

    if (!display) {
        perror("wayland-egl-probe: wl_display_connect");
        goto out;
    }
    registry = wl_display_get_registry(display);
    if (!registry || wl_registry_add_listener(registry, &registry_listener, &probe) ||
            wl_display_roundtrip(display) < 0 || !probe.compositor || !probe.wm_base) {
        fputs("wayland-egl-probe: required Wayland globals unavailable\n", stderr);
        goto out;
    }
    xdg_wm_base_add_listener(probe.wm_base, &wm_listener, &probe);
    probe.surface = wl_compositor_create_surface(probe.compositor);
    probe.xdg_surface = probe.surface
            ? xdg_wm_base_get_xdg_surface(probe.wm_base, probe.surface) : NULL;
    probe.toplevel = probe.xdg_surface
            ? xdg_surface_get_toplevel(probe.xdg_surface) : NULL;
    if (!probe.toplevel ||
            xdg_surface_add_listener(probe.xdg_surface, &surface_listener, &probe) ||
            xdg_toplevel_add_listener(probe.toplevel, &toplevel_listener, &probe)) {
        fputs("wayland-egl-probe: xdg-shell setup failed\n", stderr);
        goto out;
    }
    xdg_toplevel_set_title(probe.toplevel, "Trierarch EGL DMA-BUF probe");
    wl_surface_commit(probe.surface);
    if (wl_display_roundtrip(display) < 0 || !probe.configured) {
        fputs("wayland-egl-probe: xdg-shell configure failed\n", stderr);
        goto out;
    }

    egl_display = eglGetDisplay((EGLNativeDisplayType)display);
    EGLint major = 0;
    EGLint minor = 0;
    if (egl_display == EGL_NO_DISPLAY || !eglInitialize(egl_display, &major, &minor) ||
            !eglBindAPI(EGL_OPENGL_ES_API)) {
        fprintf(stderr, "wayland-egl-probe: EGL initialization failed: 0x%x\n", eglGetError());
        goto out;
    }
    const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE,
    };
    EGLint count = 0;
    if (!eglChooseConfig(egl_display, config_attributes, &config, 1, &count) || !count) {
        fprintf(stderr, "wayland-egl-probe: no GLES window EGL config: 0x%x\n", eglGetError());
        goto out;
    }
    window = wl_egl_window_create(probe.surface, 192, 144);
    const EGLint context_attributes[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    context = eglCreateContext(egl_display, config, EGL_NO_CONTEXT, context_attributes);
    egl_surface = window ? eglCreateWindowSurface(egl_display, config,
            (EGLNativeWindowType)window, NULL) : EGL_NO_SURFACE;
    if (context == EGL_NO_CONTEXT || egl_surface == EGL_NO_SURFACE ||
            !eglMakeCurrent(egl_display, egl_surface, egl_surface, context)) {
        fprintf(stderr, "wayland-egl-probe: EGL surface/context creation failed: 0x%x\n",
                eglGetError());
        goto out;
    }
    printf("wayland-egl-probe: EGL %d.%d vendor=%s renderer=%s version=%s\n",
            major, minor, text_or_unknown(eglQueryString(egl_display, EGL_VENDOR)),
            text_or_unknown((const char *)glGetString(GL_RENDERER)),
            text_or_unknown((const char *)glGetString(GL_VERSION)));
    fflush(stdout);

    for (unsigned frame = 0; frame < 6 && !probe.closed; ++frame) {
        const float red = frame & 1 ? 0.10f : 0.85f;
        const float green = frame & 1 ? 0.70f : 0.15f;
        glViewport(0, 0, 192, 144);
        glClearColor(red, green, 0.25f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        if (glGetError() != GL_NO_ERROR || !eglSwapBuffers(egl_display, egl_surface)) {
            fprintf(stderr, "wayland-egl-probe: GPU draw/swap failed: 0x%x\n", eglGetError());
            goto out;
        }
        if (wl_display_dispatch_pending(display) < 0)
            goto out;
        sleep_ms(160);
    }
    result = probe.closed ? EXIT_FAILURE : EXIT_SUCCESS;
    printf("wayland-egl-probe: swaps=%u closed=%d\n", 6u, probe.closed);

out:
    if (egl_display != EGL_NO_DISPLAY)
        eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (egl_surface != EGL_NO_SURFACE)
        eglDestroySurface(egl_display, egl_surface);
    if (context != EGL_NO_CONTEXT)
        eglDestroyContext(egl_display, context);
    if (egl_display != EGL_NO_DISPLAY)
        eglTerminate(egl_display);
    if (window)
        wl_egl_window_destroy(window);
    if (registry)
        wl_registry_destroy(registry);
    destroy_probe(&probe);
    if (display)
        wl_display_disconnect(display);
    return result;
}
