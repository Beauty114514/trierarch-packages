#include "compositor.h"

#include <assert.h>
#include <linux/memfd.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

struct client_state {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    int configured;
    int keymap_seen;
    int keyboard_enter_seen;
    int key_seen;
    int pointer_enter_seen;
    int pointer_motion_seen;
    int pointer_button_seen;
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

static void xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
    struct client_state *state = data;
    xdg_surface_ack_configure(surface, serial);
    state->configured = 1;
}

static const struct xdg_surface_listener xdg_surface_listener = {.configure = xdg_surface_configure};

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
        struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
    (void)pointer; (void)serial; (void)surface; (void)x; (void)y;
    ((struct client_state *)data)->pointer_enter_seen++;
}
static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
        struct wl_surface *surface) {
    (void)data; (void)pointer; (void)serial; (void)surface;
}
static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
        wl_fixed_t x, wl_fixed_t y) {
    (void)pointer; (void)time; (void)x; (void)y;
    ((struct client_state *)data)->pointer_motion_seen++;
}
static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
        uint32_t time, uint32_t button, uint32_t state_value) {
    (void)pointer; (void)serial; (void)time; (void)button; (void)state_value;
    ((struct client_state *)data)->pointer_button_seen++;
}
static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis,
        wl_fixed_t value) {
    (void)data; (void)pointer; (void)time; (void)axis; (void)value;
}
static void pointer_frame(void *data, struct wl_pointer *pointer) { (void)data; (void)pointer; }
static void pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t source) {
    (void)data; (void)pointer; (void)source;
}
static void pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis) {
    (void)data; (void)pointer; (void)time; (void)axis;
}
static void pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t discrete) {
    (void)data; (void)pointer; (void)axis; (void)discrete;
}
static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int fd, uint32_t size) {
    (void)keyboard; (void)format;
    if (fd >= 0) close(fd);
    if (size > 0) ((struct client_state *)data)->keymap_seen++;
}
static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
        struct wl_surface *surface, struct wl_array *keys) {
    (void)keyboard; (void)serial; (void)surface; (void)keys;
    ((struct client_state *)data)->keyboard_enter_seen++;
}
static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
        struct wl_surface *surface) {
    (void)data; (void)keyboard; (void)serial; (void)surface;
}
static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial,
        uint32_t time, uint32_t key, uint32_t state_value) {
    (void)keyboard; (void)serial; (void)time; (void)key; (void)state_value;
    ((struct client_state *)data)->key_seen++;
}
static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
        uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
    (void)data; (void)keyboard; (void)serial;
    (void)mods_depressed; (void)mods_latched; (void)mods_locked; (void)group;
}
static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay) {
    (void)data; (void)keyboard; (void)rate; (void)delay;
}
static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities) {
    struct client_state *state = data;
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !state->pointer) {
        state->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(state->pointer, &pointer_listener, state);
    }
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !state->keyboard) {
        state->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(state->keyboard, &keyboard_listener, state);
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
    (void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
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
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        state->seat = wl_registry_bind(registry, name, &wl_seat_interface, 5);
        wl_seat_add_listener(state->seat, &seat_listener, state);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void create_shm_buffer(struct client_state *state, struct wl_buffer **out_buffer, void **out_mapping, size_t *out_size) {
    int width = 640, height = 480, stride = width * 4;
    size_t size = (size_t)stride * height;
    int fd = (int)syscall(SYS_memfd_create, "trierarch-test-input", MFD_CLOEXEC);
    assert(fd >= 0);
    assert(ftruncate(fd, (off_t)size) == 0);
    void *mapping = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(mapping != MAP_FAILED);
    memset(mapping, 0x44, size);
    struct wl_shm_pool *pool = wl_shm_create_pool(state->shm, fd, (int32_t)size);
    assert(pool != NULL);
    *out_buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    *out_mapping = mapping;
    *out_size = size;
}

int main(void) {
    char template[] = "/tmp/trierarch-wayland-input-XXXXXX";
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
    assert(wl_display_roundtrip(state.display) >= 0);
    assert(state.compositor && state.shm && state.wm_base && state.seat);
    assert(state.pointer && state.keyboard);

    state.surface = wl_compositor_create_surface(state.compositor);
    state.xdg_surface = xdg_wm_base_get_xdg_surface(state.wm_base, state.surface);
    xdg_surface_add_listener(state.xdg_surface, &xdg_surface_listener, &state);
    state.toplevel = xdg_surface_get_toplevel(state.xdg_surface);
    wl_surface_commit(state.surface);
    while (!state.configured) assert(wl_display_dispatch(state.display) >= 0);

    struct wl_buffer *buffer = NULL;
    void *mapping = NULL;
    size_t mapping_size = 0;
    create_shm_buffer(&state, &buffer, &mapping, &mapping_size);
    wl_surface_attach(state.surface, buffer, 0, 0);
    wl_surface_damage(state.surface, 0, 0, 640, 480);
    wl_surface_commit(state.surface);
    assert(wl_display_roundtrip(state.display) >= 0);

    trierarch_pointer_move_absolute(server, 100.0f, 120.0f, 1);
    trierarch_pointer_set_button(server, 1, true, 2);
    trierarch_pointer_set_button(server, 1, false, 3);
    trierarch_keyboard_set_key(server, 30, true, 4);
    trierarch_keyboard_set_key(server, 30, false, 5);
    assert(wl_display_roundtrip(state.display) >= 0);

    assert(state.keymap_seen > 0);
    assert(state.keyboard_enter_seen > 0);
    assert(state.key_seen > 0);
    assert(state.pointer_enter_seen > 0);
    assert(state.pointer_motion_seen > 0);
    assert(state.pointer_button_seen > 0);

    wl_pointer_destroy(state.pointer);
    wl_keyboard_destroy(state.keyboard);
    wl_seat_destroy(state.seat);
    xdg_toplevel_destroy(state.toplevel);
    xdg_surface_destroy(state.xdg_surface);
    wl_surface_destroy(state.surface);
    wl_buffer_destroy(buffer);
    wl_registry_destroy(state.registry);
    wl_display_disconnect(state.display);
    munmap(mapping, mapping_size);

    stop_server = 1;
    trierarch_wayland_wake(server);
    pthread_join(thread, NULL);
    trierarch_wayland_destroy(server);
    rmdir(runtime_dir);
    puts("wayland seat input test passed");
    return 0;
}
