#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "input-method-v1-client-protocol.h"

#define MAX_COMMIT_BYTES 65536U
#define MAX_QUEUED_MESSAGES 128U
#define CONTROL_FRAME_BIT UINT32_C(0x80000000)
#define CONTROL_KEYSYM 1U
#define KEY_STATE_RELEASED 0U
#define KEY_STATE_PRESSED 1U

enum message_type { MESSAGE_TEXT, MESSAGE_KEYSYM };
struct message {
    enum message_type type;
    union { char *text; struct { uint32_t sym, state; } keysym; } value;
    struct message *next;
};
struct bridge {
    struct wl_display *display;
    struct zwp_input_method_v1 *input_method;
    struct zwp_input_method_context_v1 *context;
    struct message *head, *tail;
    uint32_t serial;
    unsigned int queued_messages;
    bool saw_input_method, have_serial, sent;
};

static volatile sig_atomic_t running = 1;
static FILE *diagnostic_log;
static void stop(int signal_number) { (void)signal_number; running = 0; }

static void trace(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    vprintf(format, arguments);
    va_end(arguments);
    if (diagnostic_log) {
        va_start(arguments, format);
        vfprintf(diagnostic_log, format, arguments);
        va_end(arguments);
        fflush(diagnostic_log);
    }
    fflush(stdout);
}

/** Discard events whose Wayland input context is no longer valid. */
static void clear_pending_messages(struct bridge *bridge, const char *reason) {
    unsigned int discarded = bridge->queued_messages;
    while (bridge->head) {
        struct message *message = bridge->head;
        bridge->head = message->next;
        if (message->type == MESSAGE_TEXT) free(message->value.text);
        free(message);
    }
    bridge->tail = NULL;
    bridge->queued_messages = 0;
    if (discarded) trace("discarded %u stale IME event(s): %s\n", discarded, reason);
}

static bool enqueue_text(struct bridge *bridge, char *text) {
    if (bridge->queued_messages == MAX_QUEUED_MESSAGES) {
        fputs("IME bridge queue is full\n", stderr); free(text); return false;
    }
    struct message *message = calloc(1, sizeof(*message));
    if (!message) { free(text); return false; }
    message->type = MESSAGE_TEXT;
    message->value.text = text;
    if (bridge->tail) bridge->tail->next = message; else bridge->head = message;
    bridge->tail = message;
    bridge->queued_messages++;
    trace("queued %zu UTF-8 bytes; queue=%u\n", strlen(text), bridge->queued_messages);
    return true;
}

static bool enqueue_keysym(struct bridge *bridge, uint32_t sym, uint32_t state) {
    if (bridge->queued_messages == MAX_QUEUED_MESSAGES) {
        fputs("IME bridge queue is full\n", stderr); return false;
    }
    struct message *message = calloc(1, sizeof(*message));
    if (!message) return false;
    message->type = MESSAGE_KEYSYM;
    message->value.keysym.sym = sym;
    message->value.keysym.state = state;
    if (bridge->tail) bridge->tail->next = message; else bridge->head = message;
    bridge->tail = message;
    bridge->queued_messages++;
    trace("queued keysym=0x%x state=%u; queue=%u\n", sym, state, bridge->queued_messages);
    return true;
}

static void try_commit(struct bridge *bridge) {
    if (!bridge->context || !bridge->have_serial || !bridge->head) return;

    unsigned int committed = 0;
    while (bridge->head) {
        struct message *message = bridge->head;
        bridge->head = message->next;
        if (!bridge->head) bridge->tail = NULL;
        bridge->queued_messages--;
        if (message->type == MESSAGE_TEXT) {
            zwp_input_method_context_v1_commit_string(bridge->context, bridge->serial, message->value.text);
            trace("committed %zu UTF-8 bytes with serial=%u; queue=%u\n",
                    strlen(message->value.text), bridge->serial, bridge->queued_messages);
            free(message->value.text);
        } else {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            uint32_t time = (uint32_t)(now.tv_sec * 1000U + now.tv_nsec / 1000000U);
            zwp_input_method_context_v1_keysym(bridge->context, bridge->serial, time,
                    message->value.keysym.sym, message->value.keysym.state, 0);
            trace("sent keysym=0x%x state=%u with serial=%u; queue=%u\n",
                    message->value.keysym.sym, message->value.keysym.state,
                    bridge->serial, bridge->queued_messages);
        }
        free(message);
        committed++;
    }
    bridge->sent = true;
    if (wl_display_flush(bridge->display) < 0 && errno != EAGAIN) {
        perror("unable to flush Wayland text commit"); running = 0;
    }
    trace("flushed %u commit(s) with serial=%u\n", committed, bridge->serial);
}

static void context_surrounding_text(void *data, struct zwp_input_method_context_v1 *context,
        const char *text, uint32_t cursor, uint32_t anchor) {
    (void)data; (void)context; (void)text; (void)cursor; (void)anchor;
}
static void context_reset(void *data, struct zwp_input_method_context_v1 *context) {
    struct bridge *bridge = data;
    if (bridge->context != context) return;
    bridge->have_serial = false;
    clear_pending_messages(bridge, "text input reset");
    trace("text input reset\n");
}
static void context_content_type(void *data, struct zwp_input_method_context_v1 *context,
        uint32_t hint, uint32_t purpose) { (void)data; (void)context; (void)hint; (void)purpose; }
static void context_invoke_action(void *data, struct zwp_input_method_context_v1 *context,
        uint32_t button, uint32_t index) { (void)data; (void)context; (void)button; (void)index; }
static void context_commit_state(void *data, struct zwp_input_method_context_v1 *context,
        uint32_t serial) {
    struct bridge *bridge = data;
    if (bridge->context != context) return;
    bridge->serial = serial; bridge->have_serial = true;
    trace("text input state serial=%u; queue=%u\n", serial, bridge->queued_messages);
    try_commit(bridge);
}
static void context_preferred_language(void *data, struct zwp_input_method_context_v1 *context,
        const char *language) { (void)data; (void)context; (void)language; }
static const struct zwp_input_method_context_v1_listener context_listener = {
    .surrounding_text = context_surrounding_text, .reset = context_reset,
    .content_type = context_content_type, .invoke_action = context_invoke_action,
    .commit_state = context_commit_state, .preferred_language = context_preferred_language,
};

static void input_method_activate(void *data, struct zwp_input_method_v1 *input_method,
        struct zwp_input_method_context_v1 *context) {
    (void)input_method;
    struct bridge *bridge = data;
    if (bridge->context) {
        clear_pending_messages(bridge, "input context replaced");
        zwp_input_method_context_v1_destroy(bridge->context);
    }
    bridge->context = context; bridge->have_serial = false;
    zwp_input_method_context_v1_add_listener(context, &context_listener, bridge);
    puts("text input activated; waiting for state serial"); fflush(stdout);
}
static void input_method_deactivate(void *data, struct zwp_input_method_v1 *input_method,
        struct zwp_input_method_context_v1 *context) {
    (void)input_method;
    struct bridge *bridge = data;
    if (bridge->context == context) {
        clear_pending_messages(bridge, "text input deactivated");
        bridge->context = NULL;
        bridge->have_serial = false;
    }
    printf("text input deactivated\n");
    fflush(stdout);
    zwp_input_method_context_v1_destroy(context);
}
static const struct zwp_input_method_v1_listener input_method_listener = {
    .activate = input_method_activate, .deactivate = input_method_deactivate,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
        const char *interface, uint32_t version) {
    struct bridge *bridge = data;
    if (strcmp(interface, zwp_input_method_v1_interface.name) || bridge->input_method || version < 1) return;
    bridge->input_method = wl_registry_bind(registry, name, &zwp_input_method_v1_interface, 1);
    if (!bridge->input_method) return;
    bridge->saw_input_method = true;
    zwp_input_method_v1_add_listener(bridge->input_method, &input_method_listener, bridge);
    puts("bound zwp_input_method_v1 version=1"); fflush(stdout);
}
static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data; (void)registry; (void)name;
}
static const struct wl_registry_listener registry_listener = {
    .global = registry_global, .global_remove = registry_global_remove,
};

static int make_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
static int listen_socket(const char *path) {
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    struct stat status;
    if (strlen(path) >= sizeof(address.sun_path)) { fputs("socket path is too long\n", stderr); return -1; }
    if (lstat(path, &status) == 0 || errno != ENOENT) {
        fprintf(stderr, "refusing to replace existing bridge socket: %s\n", path); return -1;
    }
    strcpy(address.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(fd, 1) < 0 ||
            make_nonblocking(fd) < 0) {
        perror("unable to listen on bridge socket"); if (fd >= 0) close(fd); return -1;
    }
    if (chmod(path, 0666) < 0) {
        perror("unable to set bridge socket permissions"); close(fd); unlink(path); return -1;
    }
    return fd;
}

static bool enqueue_control(struct bridge *bridge, const unsigned char *payload, size_t length) {
    if (length != 6 || payload[0] != CONTROL_KEYSYM ||
            (payload[1] != KEY_STATE_RELEASED && payload[1] != KEY_STATE_PRESSED)) {
        fputs("invalid IME bridge control frame\n", stderr); return false;
    }
    uint32_t network_sym;
    memcpy(&network_sym, payload + 2, sizeof(network_sym));
    return enqueue_keysym(bridge, ntohl(network_sym), payload[1]);
}

static bool consume_client(int fd, unsigned char *buffer, size_t *used, struct bridge *bridge) {
    for (;;) {
        ssize_t read_count = read(fd, buffer + *used, 4 + MAX_COMMIT_BYTES - *used);
        if (read_count == 0) return false;
        if (read_count < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return false;
        }
        *used += (size_t)read_count;
        while (*used >= 4) {
            uint32_t network_length;
            memcpy(&network_length, buffer, sizeof(network_length));
            uint32_t header = ntohl(network_length);
            bool control = (header & CONTROL_FRAME_BIT) != 0;
            size_t length = header & ~CONTROL_FRAME_BIT;
            if (!length || length > MAX_COMMIT_BYTES) { fputs("invalid bridge message length\n", stderr); return false; }
            if (*used < 4 + length) break;
            if (control) {
                if (!enqueue_control(bridge, buffer + 4, length)) return false;
            } else {
                if (memchr(buffer + 4, '\0', length)) { fputs("bridge commits may not contain NUL\n", stderr); return false; }
                char *text = malloc(length + 1);
                if (!text) return false;
                memcpy(text, buffer + 4, length); text[length] = '\0';
                if (!enqueue_text(bridge, text)) return false;
            }
            memmove(buffer, buffer + 4 + length, *used - 4 - length);
            *used -= 4 + length;
            try_commit(bridge);
        }
        if (*used == 4 + MAX_COMMIT_BYTES) return false;
    }
    return true;
}

static int dispatch_socket_mode(struct bridge *bridge, int listener, int *client,
        unsigned char *input, size_t *used) {
    struct pollfd fds[3] = {
        { .fd = wl_display_get_fd(bridge->display), .events = POLLIN },
        { .fd = listener, .events = POLLIN }, { .fd = *client, .events = POLLIN },
    };
    while (wl_display_prepare_read(bridge->display) != 0) {
        if (wl_display_dispatch_pending(bridge->display) < 0) return -1;
    }
    if (wl_display_flush(bridge->display) < 0 && errno != EAGAIN) return -1;
    int count;
    do { count = poll(fds, *client >= 0 ? 3 : 2, -1); } while (count < 0 && errno == EINTR && running);
    if (count < 0) { wl_display_cancel_read(bridge->display); return -1; }
    if (fds[0].revents & POLLIN) {
        if (wl_display_read_events(bridge->display) < 0) return -1;
    } else {
        wl_display_cancel_read(bridge->display);
    }
    if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
    if (wl_display_dispatch_pending(bridge->display) < 0) return -1;
    if (fds[1].revents & POLLIN) {
        int accepted = accept(listener, NULL, NULL);
        if (accepted >= 0) {
            if (make_nonblocking(accepted) < 0) close(accepted);
            else { if (*client >= 0) close(*client); *client = accepted; *used = 0; }
        }
    }
    if (*client >= 0 && fds[2].revents && !consume_client(*client, input, used, bridge)) {
        close(*client); *client = -1; *used = 0;
    }
    return 0;
}

static void usage(const char *program) {
    fprintf(stderr, "usage: %s --commit <UTF-8 text> | --socket <path>\n", program);
}
int main(int argc, char **argv) {
    bool socket_mode = argc == 3 && !strcmp(argv[1], "--socket");
    bool one_shot = argc == 3 && !strcmp(argv[1], "--commit");
    if (!socket_mode && !one_shot) { usage(argv[0]); return EXIT_FAILURE; }
    const char *log_path = getenv("TRIERARCH_IME_LOG");
    if (log_path && *log_path) diagnostic_log = fopen(log_path, "a");
    struct bridge bridge = {0};
    if (one_shot) {
        if (!argv[2][0]) { fputs("commit text may not be empty\n", stderr); return EXIT_FAILURE; }
        char *text = strdup(argv[2]);
        if (!text || !enqueue_text(&bridge, text)) return EXIT_FAILURE;
    }
    signal(SIGINT, stop); signal(SIGTERM, stop);
    bridge.display = wl_display_connect(NULL);
    if (!bridge.display) { fputs("unable to connect to WAYLAND_DISPLAY\n", stderr); return EXIT_FAILURE; }
    struct wl_registry *registry = wl_display_get_registry(bridge.display);
    wl_registry_add_listener(registry, &registry_listener, &bridge);
    if (wl_display_roundtrip(bridge.display) < 0 || !bridge.saw_input_method) {
        fputs("zwp_input_method_v1 is unavailable\n", stderr); wl_display_disconnect(bridge.display); return EXIT_FAILURE;
    }
    int listener = -1, client = -1;
    const char *socket_path = NULL;
    if (socket_mode) {
        socket_path = argv[2]; listener = listen_socket(socket_path);
        if (listener < 0) { wl_display_disconnect(bridge.display); return EXIT_FAILURE; }
        printf("listening for framed UTF-8 commits at %s\n", socket_path);
    } else {
        puts("waiting for a focused text input");
    }
    fflush(stdout);
    unsigned char input[4 + MAX_COMMIT_BYTES]; size_t used = 0;
    int result = EXIT_SUCCESS;
    if (socket_mode) {
        while (running && dispatch_socket_mode(&bridge, listener, &client, input, &used) == 0) {}
        if (running) result = EXIT_FAILURE;
    } else {
        while (!bridge.sent && wl_display_dispatch(bridge.display) >= 0) {}
        if (!bridge.sent) { fputs("Wayland connection ended before text was committed\n", stderr); result = EXIT_FAILURE; }
    }
    if (client >= 0) close(client);
    if (listener >= 0) close(listener);
    if (socket_path) unlink(socket_path);
    if (bridge.context) zwp_input_method_context_v1_destroy(bridge.context);
    clear_pending_messages(&bridge, "bridge shutdown");
    wl_display_disconnect(bridge.display);
    if (diagnostic_log) fclose(diagnostic_log);
    return result;
}
