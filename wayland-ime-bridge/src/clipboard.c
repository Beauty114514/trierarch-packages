#define _GNU_SOURCE

#include "clipboard.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-client.h>

#include "ext-data-control-v1-client-protocol.h"
#include "wlr-data-control-unstable-v1-client-protocol.h"

#define MAX_CLIPBOARD_BYTES 65536U
#define MAX_CLIPBOARD_MIMES 16
#define MIME_LENGTH 96

enum clipboard_backend { BACKEND_NONE, BACKEND_EXT, BACKEND_WLR, BACKEND_STD };

struct clipboard {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_seat *seat;
    struct ext_data_control_manager_v1 *ext_manager;
    struct zwlr_data_control_manager_v1 *wlr_manager;
    struct wl_data_device_manager *std_manager;
    enum clipboard_backend backend;

    struct ext_data_control_device_v1 *ext_device;
    struct zwlr_data_control_device_v1 *wlr_device;
    struct wl_data_device *std_device;

    struct ext_data_control_offer_v1 *ext_offer;
    struct zwlr_data_control_offer_v1 *wlr_offer;
    struct wl_data_offer *std_offer;
    bool have_selection;
    bool reading;
    bool initializing;
    char mimes[MAX_CLIPBOARD_MIMES][MIME_LENGTH];
    size_t mime_count;

    char *last_text;
    int listen_fd;
    int client_fd;
    const char *socket_path;
    FILE *log;
};

static volatile sig_atomic_t running = 1;

static void stop(int signal_number) {
    (void)signal_number;
    running = 0;
}

static void note(struct clipboard *clipboard, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    if (clipboard->log) {
        va_start(arguments, format);
        vfprintf(clipboard->log, format, arguments);
        va_end(arguments);
        fflush(clipboard->log);
    }
    fflush(stderr);
}

static const char *backend_label(enum clipboard_backend backend) {
    switch (backend) {
        case BACKEND_EXT: return "ext-data-control v1";
        case BACKEND_WLR: return "wlr-data-control v1";
        case BACKEND_STD: return "wl_data_device v3";
        default: return "none";
    }
}

static int text_mime_rank(const char *mime) {
    if (!mime) return 0;
    if (!strncasecmp(mime, "text/plain", 10)) {
        const char *parameter = mime + 10;
        if (*parameter == '\0') return 3;
        if (*parameter != ';') return 0;
        parameter++;
        while (*parameter == ' ') parameter++;
        if (!strncasecmp(parameter, "charset=", 8)) {
            const char *value = parameter + 8;
            if (!strncasecmp(value, "utf-8", 5) || !strncasecmp(value, "utf8", 4)) return 4;
        }
        return 0;
    }
    if (!strcmp(mime, "UTF8_STRING")) return 2;
    return 0;
}

static void add_mime(struct clipboard *clipboard, const char *mime);

static void drop_offer(struct clipboard *clipboard) {
    if (clipboard->ext_offer) {
        ext_data_control_offer_v1_destroy(clipboard->ext_offer);
        clipboard->ext_offer = NULL;
    }
    if (clipboard->wlr_offer) {
        zwlr_data_control_offer_v1_destroy(clipboard->wlr_offer);
        clipboard->wlr_offer = NULL;
    }
    if (clipboard->std_offer) {
        wl_data_offer_destroy(clipboard->std_offer);
        clipboard->std_offer = NULL;
    }
    clipboard->have_selection = false;
    clipboard->mime_count = 0;
}

static bool write_all(int fd, const void *data, size_t length) {
    const char *cursor = data;
    while (length) {
        ssize_t written = write(fd, cursor, length);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (written == 0) return false;
        cursor += written;
        length -= (size_t)written;
    }
    return true;
}

static void send_text(struct clipboard *clipboard) {
    if (!clipboard->last_text || clipboard->client_fd < 0) return;
    uint32_t length = (uint32_t)strlen(clipboard->last_text);
    uint32_t network_length = htonl(length);
    if (!write_all(clipboard->client_fd, &network_length, sizeof(network_length)) ||
            !write_all(clipboard->client_fd, clipboard->last_text, length)) {
        close(clipboard->client_fd);
        clipboard->client_fd = -1;
    }
}

static void accept_client(struct clipboard *clipboard) {
    int accepted = accept(clipboard->listen_fd, NULL, NULL);
    if (accepted < 0) return;
    if (clipboard->client_fd >= 0) close(clipboard->client_fd);
    clipboard->client_fd = accepted;
    note(clipboard, "clipboard: Android client connected\n");
    if (clipboard->last_text) send_text(clipboard);
}

static const char *best_mime(const struct clipboard *clipboard) {
    const char *best = NULL;
    int best_rank = 0;
    for (size_t index = 0; index < clipboard->mime_count; index++) {
        int rank = text_mime_rank(clipboard->mimes[index]);
        if (rank > best_rank) {
            best_rank = rank;
            best = clipboard->mimes[index];
        }
    }
    return best;
}

static void receive_current(struct clipboard *clipboard);

static void add_mime(struct clipboard *clipboard, const char *mime) {
    if (!mime || clipboard->mime_count >= MAX_CLIPBOARD_MIMES || !text_mime_rank(mime)) return;
    for (size_t index = 0; index < clipboard->mime_count; index++) {
        if (!strcmp(clipboard->mimes[index], mime)) return;
    }
    snprintf(clipboard->mimes[clipboard->mime_count], MIME_LENGTH, "%s", mime);
    clipboard->mime_count++;
    if (clipboard->have_selection) receive_current(clipboard);
}

static void receive_current(struct clipboard *clipboard) {
    if (!clipboard->have_selection || clipboard->reading) return;
    if (clipboard->initializing) {
        // The clipboard at session start belongs to the previous session. Do not
        // overwrite the Android clipboard until the user copies something new.
        drop_offer(clipboard);
        return;
    }
    const char *chosen = best_mime(clipboard);
    if (!chosen) return;
    char mime[MIME_LENGTH];
    snprintf(mime, sizeof(mime), "%s", chosen);

    int pipe_fds[2];
    if (pipe2(pipe_fds, O_CLOEXEC) < 0) {
        note(clipboard, "clipboard: pipe failed: %s\n", strerror(errno));
        return;
    }
    clipboard->reading = true;
    switch (clipboard->backend) {
        case BACKEND_EXT:
            if (clipboard->ext_offer) {
                ext_data_control_offer_v1_receive(clipboard->ext_offer, mime, pipe_fds[1]);
            }
            break;
        case BACKEND_WLR:
            if (clipboard->wlr_offer) {
                zwlr_data_control_offer_v1_receive(clipboard->wlr_offer, mime, pipe_fds[1]);
            }
            break;
        case BACKEND_STD:
            if (clipboard->std_offer) {
                wl_data_offer_receive(clipboard->std_offer, mime, pipe_fds[1]);
            }
            break;
        default:
            break;
    }
    close(pipe_fds[1]);
    if (wl_display_flush(clipboard->display) < 0) {
        close(pipe_fds[0]);
        clipboard->reading = false;
        running = 0;
        return;
    }

    char *buffer = malloc(MAX_CLIPBOARD_BYTES + 2);
    size_t used = 0;
    if (buffer) {
        for (;;) {
            struct pollfd descriptor = { .fd = pipe_fds[0], .events = POLLIN };
            int ready = poll(&descriptor, 1, 1000);
            if (ready <= 0) break;
            ssize_t count = read(pipe_fds[0], buffer + used, MAX_CLIPBOARD_BYTES + 1 - used);
            if (count <= 0) break;
            used += (size_t)count;
            if (used > MAX_CLIPBOARD_BYTES) break;
        }
    }
    close(pipe_fds[0]);

    if (buffer && used > 0 && used <= MAX_CLIPBOARD_BYTES) {
        buffer[used] = '\0';
        size_t length = strlen(buffer);
        if (length > 0 && (!clipboard->last_text || strcmp(clipboard->last_text, buffer))) {
            free(clipboard->last_text);
            clipboard->last_text = strdup(buffer);
            if (clipboard->last_text) {
                note(clipboard, "clipboard: forwarding %zu bytes\n", length);
                send_text(clipboard);
            }
        }
    } else if (used > MAX_CLIPBOARD_BYTES) {
        note(clipboard, "clipboard: dropped oversized selection\n");
    }
    free(buffer);
    drop_offer(clipboard);
    clipboard->reading = false;
}

/* ext-data-control */

static void ext_offer_offer(void *data, struct ext_data_control_offer_v1 *offer, const char *mime) {
    (void)offer;
    add_mime(data, mime);
}
static const struct ext_data_control_offer_v1_listener ext_offer_listener = {
    .offer = ext_offer_offer,
};

static void ext_device_data_offer(void *data, struct ext_data_control_device_v1 *device,
        struct ext_data_control_offer_v1 *offer) {
    (void)device;
    struct clipboard *clipboard = data;
    drop_offer(clipboard);
    clipboard->ext_offer = offer;
    ext_data_control_offer_v1_add_listener(offer, &ext_offer_listener, clipboard);
}
static void ext_device_selection(void *data, struct ext_data_control_device_v1 *device,
        struct ext_data_control_offer_v1 *offer) {
    (void)device;
    struct clipboard *clipboard = data;
    if (!offer) {
        drop_offer(clipboard);
        return;
    }
    clipboard->have_selection = true;
    receive_current(clipboard);
}
static void ext_device_finished(void *data, struct ext_data_control_device_v1 *device) {
    (void)data; (void)device;
    running = 0;
}
static void ext_device_primary_selection(void *data, struct ext_data_control_device_v1 *device,
        struct ext_data_control_offer_v1 *offer) {
    (void)data; (void)device; (void)offer;
}
static const struct ext_data_control_device_v1_listener ext_device_listener = {
    .data_offer = ext_device_data_offer,
    .selection = ext_device_selection,
    .finished = ext_device_finished,
    .primary_selection = ext_device_primary_selection,
};

/* wlr-data-control */

static void wlr_offer_offer(void *data, struct zwlr_data_control_offer_v1 *offer, const char *mime) {
    (void)offer;
    add_mime(data, mime);
}
static const struct zwlr_data_control_offer_v1_listener wlr_offer_listener = {
    .offer = wlr_offer_offer,
};

static void wlr_device_data_offer(void *data, struct zwlr_data_control_device_v1 *device,
        struct zwlr_data_control_offer_v1 *offer) {
    (void)device;
    struct clipboard *clipboard = data;
    drop_offer(clipboard);
    clipboard->wlr_offer = offer;
    zwlr_data_control_offer_v1_add_listener(offer, &wlr_offer_listener, clipboard);
}
static void wlr_device_selection(void *data, struct zwlr_data_control_device_v1 *device,
        struct zwlr_data_control_offer_v1 *offer) {
    (void)device;
    struct clipboard *clipboard = data;
    if (!offer) {
        drop_offer(clipboard);
        return;
    }
    clipboard->have_selection = true;
    receive_current(clipboard);
}
static void wlr_device_finished(void *data, struct zwlr_data_control_device_v1 *device) {
    (void)data; (void)device;
    running = 0;
}
static void wlr_device_primary_selection(void *data, struct zwlr_data_control_device_v1 *device,
        struct zwlr_data_control_offer_v1 *offer) {
    (void)data; (void)device; (void)offer;
}
static const struct zwlr_data_control_device_v1_listener wlr_device_listener = {
    .data_offer = wlr_device_data_offer,
    .selection = wlr_device_selection,
    .finished = wlr_device_finished,
    .primary_selection = wlr_device_primary_selection,
};

/* wl_data_device fallback */

static void std_offer_offer(void *data, struct wl_data_offer *offer, const char *mime) {
    (void)offer;
    add_mime(data, mime);
}
static void std_offer_source_actions(void *data, struct wl_data_offer *offer, uint32_t actions) {
    (void)data; (void)offer; (void)actions;
}
static void std_offer_action(void *data, struct wl_data_offer *offer, uint32_t action) {
    (void)data; (void)offer; (void)action;
}
static const struct wl_data_offer_listener std_offer_listener = {
    .offer = std_offer_offer,
    .source_actions = std_offer_source_actions,
    .action = std_offer_action,
};

static void std_device_data_offer(void *data, struct wl_data_device *device,
        struct wl_data_offer *offer) {
    (void)device;
    struct clipboard *clipboard = data;
    drop_offer(clipboard);
    clipboard->std_offer = offer;
    wl_data_offer_add_listener(offer, &std_offer_listener, clipboard);
}
static void std_device_selection(void *data, struct wl_data_device *device,
        struct wl_data_offer *offer) {
    (void)device;
    struct clipboard *clipboard = data;
    if (!offer) {
        drop_offer(clipboard);
        return;
    }
    clipboard->have_selection = true;
    receive_current(clipboard);
}
static void std_device_enter(void *data, struct wl_data_device *device, uint32_t serial,
        struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *offer) {
    (void)data; (void)device; (void)serial; (void)surface; (void)x; (void)y; (void)offer;
}
static void std_device_leave(void *data, struct wl_data_device *device) {
    (void)data; (void)device;
}
static void std_device_motion(void *data, struct wl_data_device *device, uint32_t time,
        wl_fixed_t x, wl_fixed_t y) {
    (void)data; (void)device; (void)time; (void)x; (void)y;
}
static void std_device_drop(void *data, struct wl_data_device *device) {
    (void)data; (void)device;
}
static const struct wl_data_device_listener std_device_listener = {
    .data_offer = std_device_data_offer,
    .enter = std_device_enter,
    .leave = std_device_leave,
    .motion = std_device_motion,
    .drop = std_device_drop,
    .selection = std_device_selection,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
        const char *interface, uint32_t version) {
    struct clipboard *clipboard = data;
    if (!strcmp(interface, wl_seat_interface.name) && !clipboard->seat) {
        clipboard->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    } else if (!strcmp(interface, ext_data_control_manager_v1_interface.name) &&
            !clipboard->ext_manager) {
        clipboard->ext_manager = wl_registry_bind(registry, name,
                &ext_data_control_manager_v1_interface, 1);
    } else if (!strcmp(interface, zwlr_data_control_manager_v1_interface.name) &&
            !clipboard->wlr_manager) {
        clipboard->wlr_manager = wl_registry_bind(registry, name,
                &zwlr_data_control_manager_v1_interface, 1);
    } else if (!strcmp(interface, wl_data_device_manager_interface.name) &&
            !clipboard->std_manager) {
        clipboard->std_manager = wl_registry_bind(registry, name,
                &wl_data_device_manager_interface, version < 3 ? version : 3);
    }
}
static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data; (void)registry; (void)name;
}
static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static int listen_socket(const char *path) {
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(address.sun_path)) {
        fputs("clipboard socket path is too long\n", stderr);
        return -1;
    }
    unlink(path);
    strcpy(address.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
            listen(fd, 1) < 0) {
        if (fd >= 0) close(fd);
        return -1;
    }
    if (chmod(path, 0666) < 0) {
        close(fd);
        unlink(path);
        return -1;
    }
    return fd;
}

static void destroy_clipboard(struct clipboard *clipboard) {
    if (clipboard->ext_offer) ext_data_control_offer_v1_destroy(clipboard->ext_offer);
    if (clipboard->wlr_offer) zwlr_data_control_offer_v1_destroy(clipboard->wlr_offer);
    if (clipboard->std_offer) wl_data_offer_destroy(clipboard->std_offer);
    if (clipboard->ext_device) ext_data_control_device_v1_destroy(clipboard->ext_device);
    if (clipboard->wlr_device) zwlr_data_control_device_v1_destroy(clipboard->wlr_device);
    if (clipboard->std_device) wl_data_device_release(clipboard->std_device);
    if (clipboard->ext_manager) ext_data_control_manager_v1_destroy(clipboard->ext_manager);
    if (clipboard->wlr_manager) zwlr_data_control_manager_v1_destroy(clipboard->wlr_manager);
    if (clipboard->std_manager) wl_proxy_destroy((struct wl_proxy *)clipboard->std_manager);
    if (clipboard->seat) wl_seat_destroy(clipboard->seat);
    if (clipboard->registry) wl_registry_destroy(clipboard->registry);
    if (clipboard->display) wl_display_disconnect(clipboard->display);
    free(clipboard->last_text);
    clipboard->last_text = NULL;
    if (clipboard->client_fd >= 0) close(clipboard->client_fd);
    if (clipboard->listen_fd >= 0) close(clipboard->listen_fd);
    if (clipboard->socket_path) unlink(clipboard->socket_path);
    if (clipboard->log) fclose(clipboard->log);
}

int clipboard_main(const char *socket_path) {
    signal(SIGINT, stop);
    signal(SIGTERM, stop);

    struct clipboard clipboard = {
        .backend = BACKEND_NONE,
        .listen_fd = -1,
        .client_fd = -1,
        .socket_path = socket_path,
    };
    const char *log_path = getenv("TRIERARCH_CLIPBOARD_LOG");
    if (log_path && *log_path) clipboard.log = fopen(log_path, "a");

    clipboard.display = wl_display_connect(NULL);
    if (!clipboard.display) {
        note(&clipboard, "clipboard: unable to connect to WAYLAND_DISPLAY\n");
        destroy_clipboard(&clipboard);
        return EXIT_FAILURE;
    }
    clipboard.registry = wl_display_get_registry(clipboard.display);
    wl_registry_add_listener(clipboard.registry, &registry_listener, &clipboard);
    if (wl_display_roundtrip(clipboard.display) < 0) {
        note(&clipboard, "clipboard: initial registry roundtrip failed\n");
        destroy_clipboard(&clipboard);
        return EXIT_FAILURE;
    }
    if (!clipboard.seat) {
        note(&clipboard, "clipboard: no wl_seat advertised\n");
        destroy_clipboard(&clipboard);
        return EXIT_FAILURE;
    }

    if (clipboard.ext_manager) {
        clipboard.backend = BACKEND_EXT;
        clipboard.ext_device = ext_data_control_manager_v1_get_data_device(
                clipboard.ext_manager, clipboard.seat);
        ext_data_control_device_v1_add_listener(clipboard.ext_device, &ext_device_listener,
                &clipboard);
    } else if (clipboard.wlr_manager) {
        clipboard.backend = BACKEND_WLR;
        clipboard.wlr_device = zwlr_data_control_manager_v1_get_data_device(
                clipboard.wlr_manager, clipboard.seat);
        zwlr_data_control_device_v1_add_listener(clipboard.wlr_device, &wlr_device_listener,
                &clipboard);
    } else if (clipboard.std_manager) {
        clipboard.backend = BACKEND_STD;
        clipboard.std_device = wl_data_device_manager_get_data_device(
                clipboard.std_manager, clipboard.seat);
        wl_data_device_add_listener(clipboard.std_device, &std_device_listener, &clipboard);
    } else {
        note(&clipboard, "clipboard: no supported protocol (need ext/wlr data-control or "
                "wl_data_device)\n");
        destroy_clipboard(&clipboard);
        return EXIT_SUCCESS;
    }
    note(&clipboard, "clipboard: using %s\n", backend_label(clipboard.backend));

    clipboard.listen_fd = listen_socket(socket_path);
    if (clipboard.listen_fd < 0) {
        note(&clipboard, "clipboard: unable to listen on %s: %s\n", socket_path,
                strerror(errno));
        destroy_clipboard(&clipboard);
        return EXIT_FAILURE;
    }
    note(&clipboard, "clipboard: listening at %s\n", socket_path);

    clipboard.initializing = true;
    if (wl_display_roundtrip(clipboard.display) < 0) {
        note(&clipboard, "clipboard: initial selection roundtrip failed\n");
        destroy_clipboard(&clipboard);
        return EXIT_FAILURE;
    }
    clipboard.initializing = false;

    while (running) {
        while (wl_display_prepare_read(clipboard.display) != 0) {
            if (wl_display_dispatch_pending(clipboard.display) < 0) {
                running = 0;
                break;
            }
        }
        if (!running) break;
        wl_display_flush(clipboard.display);

        struct pollfd descriptors[3];
        int count = 0;
        descriptors[count++] = (struct pollfd) {
            .fd = wl_display_get_fd(clipboard.display),
            .events = POLLIN,
        };
        descriptors[count++] = (struct pollfd) {
            .fd = clipboard.listen_fd,
            .events = POLLIN,
        };
        int client_index = -1;
        if (clipboard.client_fd >= 0) {
            client_index = count;
            descriptors[count++] = (struct pollfd) {
                .fd = clipboard.client_fd,
                .events = POLLIN,
            };
        }
        int ready = poll(descriptors, (nfds_t)count, -1);
        if (ready < 0) {
            wl_display_cancel_read(clipboard.display);
            if (errno == EINTR) continue;
            break;
        }
        if (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            wl_display_cancel_read(clipboard.display);
            break;
        }
        if (descriptors[0].revents & POLLIN) {
            if (wl_display_read_events(clipboard.display) < 0) break;
        } else {
            wl_display_cancel_read(clipboard.display);
        }
        if (wl_display_dispatch_pending(clipboard.display) < 0) break;
        if (descriptors[1].revents & POLLIN) accept_client(&clipboard);
        if (client_index >= 0 && descriptors[client_index].revents) {
            char discard[64];
            ssize_t count_read = read(clipboard.client_fd, discard, sizeof(discard));
            if (count_read <= 0) {
                close(clipboard.client_fd);
                clipboard.client_fd = -1;
            }
        }
    }

    destroy_clipboard(&clipboard);
    return EXIT_SUCCESS;
}
