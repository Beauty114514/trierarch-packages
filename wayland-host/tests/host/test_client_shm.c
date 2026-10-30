#include "compositor.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/memfd.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

struct client_state {
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    int configured;
    int buffer_released;
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

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
    struct client_state *state = data;
    xdg_surface_ack_configure(surface, serial);
    state->configured = 1;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
        const char *interface, uint32_t version) {
    struct client_state *state = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        state->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        state->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        state->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, version < 6 ? version : 6);
        xdg_wm_base_add_listener(state->wm_base, &wm_base_listener, state);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void buffer_release(void *data, struct wl_buffer *buffer) {
    (void)buffer;
    struct client_state *state = data;
    state->buffer_released++;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static int create_shm_buffer(struct client_state *state, int width, int height,
        struct wl_buffer **out_buffer, void **out_mapping, size_t *out_size) {
    int stride = width * 4;
    size_t size = (size_t)stride * height;
    int fd = (int)syscall(SYS_memfd_create, "trierarch-test-shm", MFD_CLOEXEC);
    assert(fd >= 0);
    assert(ftruncate(fd, (off_t)size) == 0);
    void *mapping = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(mapping != MAP_FAILED);
    memset(mapping, 0x33, size);
    struct wl_shm_pool *pool = wl_shm_create_pool(state->shm, fd, (int32_t)size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
            WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    *out_buffer = buffer;
    *out_mapping = mapping;
    *out_size = size;
    return 0;
}

int main(void) {
    char template[] = "/tmp/trierarch-wayland-client-XXXXXX";
    char *runtime_dir = mkdtemp(template);
    assert(runtime_dir != NULL);

    server = trierarch_wayland_create(runtime_dir, false);
    assert(server != NULL);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, server_thread, NULL) == 0);

    struct client_state state = {0};
    state.display = wl_display_connect("wayland-trierarch");
    assert(state.display != NULL);
    struct wl_registry *registry = wl_display_get_registry(state.display);
    wl_registry_add_listener(registry, &registry_listener, &state);
    assert(wl_display_roundtrip(state.display) >= 0);
    assert(state.compositor && state.shm && state.wm_base);

    state.surface = wl_compositor_create_surface(state.compositor);
    state.xdg_surface = xdg_wm_base_get_xdg_surface(state.wm_base, state.surface);
    xdg_surface_add_listener(state.xdg_surface, &xdg_surface_listener, &state);
    state.toplevel = xdg_surface_get_toplevel(state.xdg_surface);
    wl_surface_commit(state.surface);

    while (!state.configured) assert(wl_display_dispatch(state.display) >= 0);

    struct wl_buffer *buffer = NULL;
    void *mapping = NULL;
    size_t mapping_size = 0;
    create_shm_buffer(&state, 640, 480, &buffer, &mapping, &mapping_size);
    wl_buffer_add_listener(buffer, &buffer_listener, &state);
    wl_surface_attach(state.surface, buffer, 0, 0);
    wl_surface_damage(state.surface, 0, 0, 640, 480);
    wl_surface_commit(state.surface);
    assert(wl_display_roundtrip(state.display) >= 0);

    xdg_toplevel_destroy(state.toplevel);
    xdg_surface_destroy(state.xdg_surface);
    wl_surface_destroy(state.surface);
    assert(wl_display_roundtrip(state.display) >= 0);
    assert(state.buffer_released == 1);
    wl_buffer_destroy(buffer);
    wl_display_disconnect(state.display);

    stop_server = 1;
    trierarch_wayland_wake(server);
    pthread_join(thread, NULL);
    trierarch_wayland_destroy(server);
    munmap(mapping, mapping_size);
    rmdir(runtime_dir);
    puts("wayland client SHM test passed");
    return 0;
}
