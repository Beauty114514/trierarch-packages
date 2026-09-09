#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

#include "input-method-v1-client-protocol.h"

struct bridge {
    struct wl_display *display;
    struct zwp_input_method_v1 *input_method;
    struct zwp_input_method_context_v1 *context;
    uint32_t serial;
    bool saw_input_method;
    bool have_serial;
    bool sent;
    const char *commit_text;
};

static void try_commit(struct bridge *bridge) {
    if (!bridge->context || !bridge->have_serial || bridge->sent) return;
    zwp_input_method_context_v1_commit_string(bridge->context, bridge->serial, bridge->commit_text);
    wl_display_flush(bridge->display);
    bridge->sent = true;
    printf("committed %zu UTF-8 bytes with serial=%u\n", strlen(bridge->commit_text), bridge->serial);
    fflush(stdout);
}

static void context_surrounding_text(void *data, struct zwp_input_method_context_v1 *context,
        const char *text, uint32_t cursor, uint32_t anchor) {
    (void)data; (void)context; (void)text; (void)cursor; (void)anchor;
}

static void context_reset(void *data, struct zwp_input_method_context_v1 *context) {
    (void)data; (void)context;
}

static void context_content_type(void *data, struct zwp_input_method_context_v1 *context,
        uint32_t hint, uint32_t purpose) {
    (void)data; (void)context; (void)hint; (void)purpose;
}

static void context_invoke_action(void *data, struct zwp_input_method_context_v1 *context,
        uint32_t button, uint32_t index) {
    (void)data; (void)context; (void)button; (void)index;
}

static void context_commit_state(void *data, struct zwp_input_method_context_v1 *context,
        uint32_t serial) {
    (void)context;
    struct bridge *bridge = data;
    bridge->serial = serial;
    bridge->have_serial = true;
    printf("text input state serial=%u\n", serial);
    try_commit(bridge);
}

static void context_preferred_language(void *data, struct zwp_input_method_context_v1 *context,
        const char *language) {
    (void)data; (void)context; (void)language;
}

static const struct zwp_input_method_context_v1_listener context_listener = {
    .surrounding_text = context_surrounding_text,
    .reset = context_reset,
    .content_type = context_content_type,
    .invoke_action = context_invoke_action,
    .commit_state = context_commit_state,
    .preferred_language = context_preferred_language,
};

static void input_method_activate(void *data, struct zwp_input_method_v1 *input_method,
        struct zwp_input_method_context_v1 *context) {
    (void)input_method;
    struct bridge *bridge = data;
    if (bridge->context) zwp_input_method_context_v1_destroy(bridge->context);
    bridge->context = context;
    bridge->have_serial = false;
    zwp_input_method_context_v1_add_listener(context, &context_listener, bridge);
    puts("text input activated; waiting for state serial");
    fflush(stdout);
}

static void input_method_deactivate(void *data, struct zwp_input_method_v1 *input_method,
        struct zwp_input_method_context_v1 *context) {
    (void)input_method;
    struct bridge *bridge = data;
    if (bridge->context == context) {
        bridge->context = NULL;
        bridge->have_serial = false;
    }
    zwp_input_method_context_v1_destroy(context);
}

static const struct zwp_input_method_v1_listener input_method_listener = {
    .activate = input_method_activate,
    .deactivate = input_method_deactivate,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
        const char *interface, uint32_t version) {
    struct bridge *bridge = data;
    if (strcmp(interface, zwp_input_method_v1_interface.name) != 0 || bridge->input_method || version < 1) return;
    bridge->input_method = wl_registry_bind(registry, name, &zwp_input_method_v1_interface, 1);
    if (!bridge->input_method) return;
    bridge->saw_input_method = true;
    zwp_input_method_v1_add_listener(bridge->input_method, &input_method_listener, bridge);
    puts("bound zwp_input_method_v1 version=1");
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void usage(const char *program) {
    fprintf(stderr, "usage: %s --commit <UTF-8 text>\n", program);
}

int main(int argc, char **argv) {
    if (argc != 3 || strcmp(argv[1], "--commit") != 0) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    struct bridge bridge = { .commit_text = argv[2] };
    bridge.display = wl_display_connect(NULL);
    if (!bridge.display) {
        fputs("unable to connect to WAYLAND_DISPLAY\n", stderr);
        return EXIT_FAILURE;
    }

    struct wl_registry *registry = wl_display_get_registry(bridge.display);
    wl_registry_add_listener(registry, &registry_listener, &bridge);
    if (wl_display_roundtrip(bridge.display) < 0 || !bridge.saw_input_method) {
        fputs("zwp_input_method_v1 is unavailable\n", stderr);
        wl_display_disconnect(bridge.display);
        return EXIT_FAILURE;
    }

    puts("waiting for a focused text input");
    fflush(stdout);
    while (!bridge.sent && wl_display_dispatch(bridge.display) >= 0) {}

    if (!bridge.sent) {
        fputs("Wayland connection ended before text was committed\n", stderr);
        wl_display_disconnect(bridge.display);
        return EXIT_FAILURE;
    }

    int result = wl_display_roundtrip(bridge.display) < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    if (bridge.context) zwp_input_method_context_v1_destroy(bridge.context);
    wl_display_disconnect(bridge.display);
    return result;
}
