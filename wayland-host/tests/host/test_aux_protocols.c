#include "compositor.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

struct client_state {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct xdg_wm_base *wm_base;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct wp_viewporter *viewporter;
    struct zxdg_decoration_manager_v1 *decoration_manager;
    int layer_configured;
    int decoration_configured;
    uint32_t decoration_mode;
};

static struct wayland_server *server;
static volatile int stop_server;

static void *server_thread(void *unused) {
    (void)unused;
    while (!stop_server) trierarch_wayland_dispatch(server);
    return NULL;
}

static void wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(base, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = {.ping = wm_base_ping};

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *surface,
        uint32_t serial, uint32_t width, uint32_t height) {
    (void)width; (void)height;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    ((struct client_state *)data)->layer_configured++;
}
static void layer_closed(void *data, struct zwlr_layer_surface_v1 *surface) {
    (void)data; (void)surface;
}
static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

static void decoration_configure(void *data, struct zxdg_toplevel_decoration_v1 *decoration,
        uint32_t mode) {
    (void)decoration;
    struct client_state *state = data;
    state->decoration_configured++;
    state->decoration_mode = mode;
}
static const struct zxdg_toplevel_decoration_v1_listener decoration_listener = {
    .configure = decoration_configure,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
        const char *interface, uint32_t version) {
    struct client_state *state = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        state->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        state->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, version < 6 ? version : 6);
        xdg_wm_base_add_listener(state->wm_base, &wm_base_listener, state);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        state->layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
        state->viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
    } else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
        state->decoration_manager = wl_registry_bind(registry, name,
                &zxdg_decoration_manager_v1_interface, 1);
    }
}
static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data; (void)registry; (void)name;
}
static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

int main(void) {
    char template[] = "/tmp/trierarch-wayland-aux-XXXXXX";
    char *runtime_dir = mkdtemp(template);
    assert(runtime_dir != NULL);
    server = trierarch_wayland_create(runtime_dir, false);
    assert(server != NULL);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, server_thread, NULL) == 0);

    struct client_state state = {0};
    state.display = wl_display_connect("wayland-trierarch");
    assert(state.display != NULL);
    state.registry = wl_display_get_registry(state.display);
    wl_registry_add_listener(state.registry, &registry_listener, &state);
    assert(wl_display_roundtrip(state.display) >= 0);
    assert(state.compositor && state.wm_base);
    assert(state.layer_shell && state.viewporter && state.decoration_manager);

    /* Layer-shell: commit without a buffer must still produce configure. */
    struct wl_surface *layer_wl_surface = wl_compositor_create_surface(state.compositor);
    struct zwlr_layer_surface_v1 *layer_surface = zwlr_layer_shell_v1_get_layer_surface(
            state.layer_shell, layer_wl_surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "test");
    assert(layer_surface != NULL);
    zwlr_layer_surface_v1_add_listener(layer_surface, &layer_listener, &state);
    zwlr_layer_surface_v1_set_size(layer_surface, 320, 200);
    zwlr_layer_surface_v1_set_anchor(layer_surface,
            ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
    zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface,
            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    wl_surface_commit(layer_wl_surface);
    while (!state.layer_configured) assert(wl_display_dispatch(state.display) >= 0);
    assert(state.layer_configured > 0);

    /* Viewporter: set source/destination; a protocol error would fail roundtrip. */
    struct wl_surface *viewport_wl_surface = wl_compositor_create_surface(state.compositor);
    struct wp_viewport *viewport = wp_viewporter_get_viewport(state.viewporter, viewport_wl_surface);
    assert(viewport != NULL);
    wp_viewport_set_source(viewport, wl_fixed_from_int(0), wl_fixed_from_int(0),
            wl_fixed_from_int(640), wl_fixed_from_int(480));
    wp_viewport_set_destination(viewport, 320, 240);
    wl_surface_commit(viewport_wl_surface);
    assert(wl_display_roundtrip(state.display) >= 0);

    /* xdg-decoration: server-side mode must be announced immediately. */
    struct wl_surface *decorated_wl_surface = wl_compositor_create_surface(state.compositor);
    struct xdg_surface *xdg_surface = xdg_wm_base_get_xdg_surface(state.wm_base, decorated_wl_surface);
    struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg_surface);
    struct zxdg_toplevel_decoration_v1 *decoration =
            zxdg_decoration_manager_v1_get_toplevel_decoration(state.decoration_manager, toplevel);
    assert(decoration != NULL);
    zxdg_toplevel_decoration_v1_add_listener(decoration, &decoration_listener, &state);
    assert(wl_display_roundtrip(state.display) >= 0);
    assert(state.decoration_configured > 0);
    assert(state.decoration_mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);

    zwlr_layer_surface_v1_destroy(layer_surface);
    wl_surface_destroy(layer_wl_surface);
    wp_viewport_destroy(viewport);
    wl_surface_destroy(viewport_wl_surface);
    zxdg_toplevel_decoration_v1_destroy(decoration);
    xdg_toplevel_destroy(toplevel);
    xdg_surface_destroy(xdg_surface);
    wl_surface_destroy(decorated_wl_surface);
    zxdg_decoration_manager_v1_destroy(state.decoration_manager);
    wp_viewporter_destroy(state.viewporter);
    zwlr_layer_shell_v1_destroy(state.layer_shell);
    xdg_wm_base_destroy(state.wm_base);
    wl_compositor_destroy(state.compositor);
    wl_registry_destroy(state.registry);
    wl_display_disconnect(state.display);

    stop_server = 1;
    trierarch_wayland_wake(server);
    pthread_join(thread, NULL);
    trierarch_wayland_destroy(server);
    rmdir(runtime_dir);
    puts("wayland aux protocol test passed");
    return 0;
}
