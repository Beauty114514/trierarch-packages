#include "server_internal.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void seat_release(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void pointer_set_cursor(struct wl_client *client, struct wl_resource *resource,
        uint32_t serial, struct wl_resource *surface, int32_t hotspot_x,
        int32_t hotspot_y) {
    (void)client;
    (void)resource;
    (void)serial;
    (void)surface;
    (void)hotspot_x;
    (void)hotspot_y;
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
    wl_resource_set_implementation(resource, &pointer_impl, NULL, NULL);
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
