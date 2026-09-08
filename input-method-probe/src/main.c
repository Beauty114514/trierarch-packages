#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

#include "input-method-v1-client-protocol.h"

struct probe {
    struct wl_display *display;
    struct zwp_input_method_v1 *input_method;
    struct zwp_input_method_context_v1 *context;
    bool saw_input_method;
};

static volatile sig_atomic_t keep_running = 1;

static void stop(int signal_number) {
    (void)signal_number;
    keep_running = 0;
}

static void context_surrounding_text(void *data,
        struct zwp_input_method_context_v1 *context, const char *text,
        uint32_t cursor, uint32_t anchor) {
    (void)data;
    (void)context;
    printf("surrounding_text cursor=%u anchor=%u text=%s\n", cursor, anchor, text ? text : "");
}

static void context_reset(void *data, struct zwp_input_method_context_v1 *context) {
    (void)data;
    (void)context;
    puts("reset");
}

static void context_content_type(void *data, struct zwp_input_method_context_v1 *context,
        uint32_t hint, uint32_t purpose) {
    (void)data;
    (void)context;
    printf("content_type hint=%u purpose=%u\n", hint, purpose);
}

static void context_invoke_action(void *data, struct zwp_input_method_context_v1 *context,
        uint32_t button, uint32_t index) {
    (void)data;
    (void)context;
    printf("invoke_action button=%u index=%u\n", button, index);
}

static void context_commit_state(void *data, struct zwp_input_method_context_v1 *context,
        uint32_t serial) {
    (void)data;
    (void)context;
    printf("commit_state serial=%u\n", serial);
}

static void context_preferred_language(void *data,
        struct zwp_input_method_context_v1 *context, const char *language) {
    (void)data;
    (void)context;
    printf("preferred_language=%s\n", language ? language : "");
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
    struct probe *probe = data;
    if (probe->context) zwp_input_method_context_v1_destroy(probe->context);
    probe->context = context;
    zwp_input_method_context_v1_add_listener(context, &context_listener, probe);
    puts("activate");
    fflush(stdout);
}

static void input_method_deactivate(void *data, struct zwp_input_method_v1 *input_method,
        struct zwp_input_method_context_v1 *context) {
    (void)input_method;
    struct probe *probe = data;
    puts("deactivate");
    if (probe->context == context) probe->context = NULL;
    zwp_input_method_context_v1_destroy(context);
    fflush(stdout);
}

static const struct zwp_input_method_v1_listener input_method_listener = {
    .activate = input_method_activate,
    .deactivate = input_method_deactivate,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
        const char *interface, uint32_t version) {
    struct probe *probe = data;
    if (strcmp(interface, zwp_input_method_v1_interface.name) != 0) return;
    if (version < 1 || probe->input_method) return;
    probe->input_method = wl_registry_bind(registry, name, &zwp_input_method_v1_interface, 1);
    if (!probe->input_method) return;
    probe->saw_input_method = true;
    zwp_input_method_v1_add_listener(probe->input_method, &input_method_listener, probe);
    puts("bound zwp_input_method_v1 version=1");
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

int main(void) {
    struct probe probe = {0};
    signal(SIGINT, stop);
    signal(SIGTERM, stop);

    probe.display = wl_display_connect(NULL);
    if (!probe.display) {
        fputs("unable to connect to WAYLAND_DISPLAY\n", stderr);
        return EXIT_FAILURE;
    }
    struct wl_registry *registry = wl_display_get_registry(probe.display);
    wl_registry_add_listener(registry, &registry_listener, &probe);
    if (wl_display_roundtrip(probe.display) < 0 || !probe.saw_input_method) {
        fputs("zwp_input_method_v1 is unavailable\n", stderr);
        wl_display_disconnect(probe.display);
        return EXIT_FAILURE;
    }
    puts("waiting for text-input activation; press Ctrl-C to stop");
    fflush(stdout);
    while (keep_running && wl_display_dispatch(probe.display) >= 0) {}

    if (probe.context) zwp_input_method_context_v1_destroy(probe.context);
    wl_display_disconnect(probe.display);
    return EXIT_SUCCESS;
}
