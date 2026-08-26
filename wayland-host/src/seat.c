#include "server_internal.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BTN_LEFT 0x110
#define BTN_MIDDLE 0x112
#define BTN_RIGHT 0x111

static void pointer_resource_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct pointer_resource *tracked = wl_container_of(listener, tracked, destroy_listener);
    wl_list_remove(&tracked->link);
    wl_list_remove(&tracked->destroy_listener.link);
    free(tracked);
}

static void track_pointer_resource(struct wayland_server *server, struct wl_resource *resource) {
    if (!server || !resource) return;
    struct pointer_resource *tracked = calloc(1, sizeof(*tracked));
    if (!tracked) return;
    tracked->resource = resource;
    tracked->destroy_listener.notify = pointer_resource_destroy;
    wl_resource_add_destroy_listener(resource, &tracked->destroy_listener);
    wl_list_insert(&server->pointer_resources, &tracked->link);
}

static struct compositor_surface *find_pointer_target(struct wayland_server *server) {
    struct compositor_surface *best = NULL;
    int64_t best_area = -1;
    struct compositor_surface *surface;
    wl_list_for_each(surface, &server->surfaces, link) {
        if (surface->parent || !surface->xdg_toplevel || !surface->mapped || !surface->current)
            continue;
        int64_t area = (int64_t)surface->width * (int64_t)surface->height;
        if (area > best_area) {
            best = surface;
            best_area = area;
        }
    }
    return best;
}

static void send_pointer_enter(struct wayland_server *server, struct compositor_surface *surface) {
    if (!surface || !surface->wl_surface) return;
    struct wl_client *client = wl_resource_get_client(surface->wl_surface);
    uint32_t serial = wl_display_next_serial(server->display);
    struct pointer_resource *tracked;
    wl_list_for_each(tracked, &server->pointer_resources, link) {
        if (wl_resource_get_client(tracked->resource) == client)
            wl_pointer_send_enter(tracked->resource, serial, surface->wl_surface,
                    server->pointer_x, server->pointer_y);
    }
}

static void send_pointer_leave(struct wayland_server *server, struct compositor_surface *surface) {
    if (!surface || !surface->wl_surface) return;
    struct wl_client *client = wl_resource_get_client(surface->wl_surface);
    uint32_t serial = wl_display_next_serial(server->display);
    struct pointer_resource *tracked;
    wl_list_for_each(tracked, &server->pointer_resources, link) {
        if (wl_resource_get_client(tracked->resource) == client)
            wl_pointer_send_leave(tracked->resource, serial, surface->wl_surface);
    }
}

static void update_pointer_focus(struct wayland_server *server) {
    struct compositor_surface *target = find_pointer_target(server);
    if (target == server->pointer_focus) return;
    send_pointer_leave(server, server->pointer_focus);
    server->pointer_focus = target;
    send_pointer_enter(server, target);
}

static void send_pointer_motion(struct wayland_server *server, uint32_t time_ms) {
    update_pointer_focus(server);
    if (!server->pointer_focus || !server->pointer_focus->wl_surface) return;
    struct wl_client *client = wl_resource_get_client(server->pointer_focus->wl_surface);
    struct pointer_resource *tracked;
    wl_list_for_each(tracked, &server->pointer_resources, link) {
        if (wl_resource_get_client(tracked->resource) == client) {
            wl_pointer_send_motion(tracked->resource, time_ms, server->pointer_x, server->pointer_y);
            if (wl_resource_get_version(tracked->resource) >= 5)
                wl_pointer_send_frame(tracked->resource);
        }
    }
}

void trierarch_pointer_move_absolute(struct wayland_server *server,
        float x, float y, uint32_t time_ms) {
    if (!server) return;
    if (server->output_width > 0) x = x < 0 ? 0 : (x > server->output_width ? server->output_width : x);
    if (server->output_height > 0) y = y < 0 ? 0 : (y > server->output_height ? server->output_height : y);
    server->pointer_x = wl_fixed_from_double(x);
    server->pointer_y = wl_fixed_from_double(y);
    send_pointer_motion(server, time_ms);
}

void trierarch_pointer_move_relative(struct wayland_server *server,
        float delta_x, float delta_y, uint32_t time_ms) {
    if (!server) return;
    trierarch_pointer_move_absolute(server,
            (float)wl_fixed_to_double(server->pointer_x) + delta_x,
            (float)wl_fixed_to_double(server->pointer_y) + delta_y, time_ms);
}

void trierarch_pointer_set_button(struct wayland_server *server,
        int button, bool pressed, uint32_t time_ms) {
    if (!server) return;
    uint32_t bit = button >= 1 && button <= 3 ? 1u << (button - 1) : 0;
    if (!bit || (!!(server->pointer_buttons & bit) == pressed)) return;
    update_pointer_focus(server);
    if (!server->pointer_focus || !server->pointer_focus->wl_surface) {
        if (!pressed) server->pointer_buttons &= ~bit;
        return;
    }
    uint32_t linux_button = button == 2 ? BTN_MIDDLE : button == 3 ? BTN_RIGHT : BTN_LEFT;
    struct wl_client *client = wl_resource_get_client(server->pointer_focus->wl_surface);
    uint32_t serial = wl_display_next_serial(server->display);
    struct pointer_resource *tracked;
    wl_list_for_each(tracked, &server->pointer_resources, link) {
        if (wl_resource_get_client(tracked->resource) == client) {
            wl_pointer_send_button(tracked->resource, serial, time_ms, linux_button,
                    pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
            if (wl_resource_get_version(tracked->resource) >= 5)
                wl_pointer_send_frame(tracked->resource);
        }
    }
    if (pressed) server->pointer_buttons |= bit;
    else server->pointer_buttons &= ~bit;
}

void trierarch_pointer_scroll(struct wayland_server *server,
        float delta_x, float delta_y, uint32_t time_ms) {
    if (!server) return;
    update_pointer_focus(server);
    if (!server->pointer_focus || !server->pointer_focus->wl_surface) return;
    struct wl_client *client = wl_resource_get_client(server->pointer_focus->wl_surface);
    struct pointer_resource *tracked;
    wl_list_for_each(tracked, &server->pointer_resources, link) {
        if (wl_resource_get_client(tracked->resource) != client) continue;
        if (delta_y != 0) wl_pointer_send_axis(tracked->resource, time_ms, WL_POINTER_AXIS_VERTICAL_SCROLL,
                wl_fixed_from_double(delta_y));
        if (delta_x != 0) wl_pointer_send_axis(tracked->resource, time_ms, WL_POINTER_AXIS_HORIZONTAL_SCROLL,
                wl_fixed_from_double(delta_x));
        if (wl_resource_get_version(tracked->resource) >= 5)
            wl_pointer_send_frame(tracked->resource);
    }
}

void trierarch_pointer_reset(struct wayland_server *server, uint32_t time_ms) {
    if (!server) return;
    for (int button = 1; button <= 3; button++) {
        if (server->pointer_buttons & (1u << (button - 1)))
            trierarch_pointer_set_button(server, button, false, time_ms);
    }
}

void trierarch_pointer_set_cursor_visible(wayland_server_t *opaque, bool visible) {
    struct wayland_server *server = (struct wayland_server *)opaque;
    if (server) server->cursor_visible = visible;
}

static void seat_release(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void pointer_set_cursor(struct wl_client *client, struct wl_resource *resource,
        uint32_t serial, struct wl_resource *surface, int32_t hotspot_x,
        int32_t hotspot_y) {
    (void)serial;
    struct wayland_server *server = wl_resource_get_user_data(resource);
    if (!server) return;
    if (server->cursor_surface) server->cursor_surface->is_cursor = false;
    server->cursor_surface = NULL;
    server->cursor_hotspot_x = 0;
    server->cursor_hotspot_y = 0;
    if (!surface) return;
    if (wl_resource_get_client(surface) != client) return;
    struct compositor_surface *cursor = trierarch_surface_from_resource(surface);
    if (!cursor || cursor->server != server) return;
    cursor->is_cursor = true;
    server->cursor_surface = cursor;
    server->cursor_hotspot_x = hotspot_x;
    server->cursor_hotspot_y = hotspot_y;
}

static const struct wl_pointer_interface pointer_impl = {
    .set_cursor = pointer_set_cursor,
    .release = seat_release,
};

static void send_keymap(struct wl_resource *resource) {
    static const char keymap[] =
        "xkb_keymap {\n"
        " xkb_keycodes { minimum = 8; maximum = 255; };\n"
        " xkb_types { };\n"
        " xkb_compatibility { };\n"
        " xkb_symbols { };\n"
        "};\n";
    char path[] = "/tmp/trierarch-keymap-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return;
    unlink(path);
    size_t length = sizeof(keymap) - 1;
    if (write(fd, keymap, length) != (ssize_t)length) {
        close(fd);
        return;
    }
    lseek(fd, 0, SEEK_SET);
    wl_keyboard_send_keymap(resource, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
            fd, (uint32_t)length);
    close(fd);
}

static const struct wl_keyboard_interface keyboard_impl = {
    .release = seat_release,
};

static const struct wl_touch_interface touch_impl = {
    .release = seat_release,
};

static void seat_get_pointer(struct wl_client *client, struct wl_resource *seat,
        uint32_t id) {
    uint32_t version = (uint32_t)wl_resource_get_version(seat);
    struct wl_resource *resource = wl_resource_create(client, &wl_pointer_interface,
            version < 10 ? version : 10, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    struct wayland_server *server = wl_resource_get_user_data(seat);
    wl_resource_set_implementation(resource, &pointer_impl, server, NULL);
    track_pointer_resource(server, resource);
}

static void seat_get_keyboard(struct wl_client *client, struct wl_resource *seat,
        uint32_t id) {
    uint32_t version = (uint32_t)wl_resource_get_version(seat);
    struct wl_resource *resource = wl_resource_create(client, &wl_keyboard_interface,
            version < 10 ? version : 10, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &keyboard_impl, NULL, NULL);
}

static void seat_get_touch(struct wl_client *client, struct wl_resource *seat,
        uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client, &wl_touch_interface,
            wl_resource_get_version(seat), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &touch_impl, NULL, NULL);
}

static const struct wl_seat_interface seat_impl = {
    .get_pointer = seat_get_pointer,
    .get_keyboard = seat_get_keyboard,
    .get_touch = seat_get_touch,
    .release = seat_release,
};

void trierarch_seat_bind(struct wl_client *client, void *data,
        uint32_t version, uint32_t id) {
    struct wayland_server *server = data;
    uint32_t bind_version = version < 7 ? version : 7;
    struct wl_resource *resource = wl_resource_create(client, &wl_seat_interface,
            bind_version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &seat_impl, server, NULL);
    wl_seat_send_capabilities(resource,
            WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD |
            WL_SEAT_CAPABILITY_TOUCH);
    if (bind_version >= 2) wl_seat_send_name(resource, "Trierarch seat");
}
