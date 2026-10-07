#define _GNU_SOURCE
#include "presenter_bridge.h"

#include "server_internal.h"
#include "trierarch_presenter_protocol.h"

#include <android/log.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define TAG "TrierarchPresenter"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

enum presenter_peer_state {
    PRESENTER_PEER_AWAIT_HELLO,
    PRESENTER_PEER_READY,
};

union presenter_message {
    struct trierarch_presenter_header header;
    struct trierarch_presenter_hello hello;
    struct trierarch_presenter_buffer buffer;
    struct trierarch_presenter_render render;
    struct trierarch_presenter_frame_done frame_done;
    struct trierarch_presenter_reset reset;
    struct trierarch_presenter_error error;
};

struct trierarch_presenter_bridge {
    struct wayland_server *server;
    int listener_fd;
    int client_fd;
    struct wl_event_source *listener_source;
    struct wl_event_source *client_source;
    enum presenter_peer_state peer_state;
    char socket_path[PATH_MAX];
};

static bool valid_header(const struct trierarch_presenter_header *header) {
    return header && header->magic == TRIERARCH_PRESENTER_MAGIC &&
            header->version == TRIERARCH_PRESENTER_VERSION;
}

static void close_received_fds(struct msghdr *packet) {
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(packet); cmsg;
            cmsg = CMSG_NXTHDR(packet, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
            continue;
        size_t bytes = cmsg->cmsg_len - CMSG_LEN(0);
        int *fds = (int *)CMSG_DATA(cmsg);
        for (size_t index = 0; index < bytes / sizeof(*fds); ++index)
            if (fds[index] >= 0)
                close(fds[index]);
    }
}

static void close_client(struct trierarch_presenter_bridge *bridge) {
    if (bridge->client_source) {
        wl_event_source_remove(bridge->client_source);
        bridge->client_source = NULL;
    }
    if (bridge->client_fd >= 0)
        close(bridge->client_fd);
    bridge->client_fd = -1;
    bridge->peer_state = PRESENTER_PEER_AWAIT_HELLO;
}

static void send_error(struct trierarch_presenter_bridge *bridge, uint32_t code) {
    if (bridge->client_fd < 0)
        return;
    const struct trierarch_presenter_error error = {
        .header = {
            .magic = TRIERARCH_PRESENTER_MAGIC,
            .version = TRIERARCH_PRESENTER_VERSION,
            .type = TRIERARCH_PRESENTER_ERROR,
        },
        .code = code,
    };
    (void)send(bridge->client_fd, &error, sizeof(error), MSG_NOSIGNAL);
}

static int receive_packet(struct trierarch_presenter_bridge *bridge,
        union presenter_message *message, size_t *message_size) {
    char control[CMSG_SPACE(sizeof(int) * 4)] = {0};
    struct iovec iov = { .iov_base = message, .iov_len = sizeof(*message) };
    struct msghdr packet = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = control,
        .msg_controllen = sizeof(control),
    };
    ssize_t received = recvmsg(bridge->client_fd, &packet,
            MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return 0;
    if (received <= 0 || (packet.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) {
        close_received_fds(&packet);
        return -1;
    }
    close_received_fds(&packet);
    *message_size = (size_t)received;
    return 1;
}

static int client_readable(int fd, uint32_t mask, void *data) {
    (void)fd;
    struct trierarch_presenter_bridge *bridge = data;
    if (!bridge)
        return 0;
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        close_client(bridge);
        return 0;
    }
    if (!(mask & WL_EVENT_READABLE))
        return 0;

    union presenter_message message = {0};
    size_t message_size = 0;
    int result = receive_packet(bridge, &message, &message_size);
    if (result == 0)
        return 0;
    if (result < 0) {
        LOGW("presenter peer disconnected or sent a truncated packet");
        close_client(bridge);
        return 0;
    }

    if (bridge->peer_state == PRESENTER_PEER_AWAIT_HELLO) {
        if (message_size == sizeof(message.hello) &&
                valid_header(&message.hello.header) &&
                message.hello.header.type == TRIERARCH_PRESENTER_HELLO) {
            bridge->peer_state = PRESENTER_PEER_READY;
            LOGI("presenter peer handshake complete");
            return 0;
        }
        LOGW("presenter peer sent a message before HELLO");
        send_error(bridge, EPROTO);
        close_client(bridge);
        return 0;
    }

    /* Buffer dequeue/queue is deliberately absent from this step.  Reject
     * frame traffic rather than letting a future backend mistake a handshake
     * socket for an active presenter. */
    if (message_size < sizeof(message.header) ||
            !valid_header(&message.header)) {
        LOGW("presenter peer sent an invalid protocol header");
        send_error(bridge, EPROTO);
        close_client(bridge);
        return 0;
    }
    LOGW("presenter peer requested type=%u before slots are implemented",
            message.header.type);
    send_error(bridge, ENOTSUP);
    return 0;
}

static int listener_readable(int fd, uint32_t mask, void *data) {
    (void)fd;
    struct trierarch_presenter_bridge *bridge = data;
    if (!bridge || !(mask & WL_EVENT_READABLE) || bridge->client_fd >= 0)
        return 0;
    int client_fd = accept4(bridge->listener_fd, NULL, NULL,
            SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (client_fd < 0)
        return 0;
    bridge->client_fd = client_fd;
    bridge->client_source = wl_event_loop_add_fd(bridge->server->event_loop,
            client_fd, WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR,
            client_readable, bridge);
    if (!bridge->client_source)
        close_client(bridge);
    return 0;
}

struct trierarch_presenter_bridge *trierarch_presenter_bridge_create(
        struct wayland_server *server, const char *runtime_dir) {
    if (!server || !server->event_loop || !runtime_dir)
        return NULL;
    struct trierarch_presenter_bridge *bridge = calloc(1, sizeof(*bridge));
    if (!bridge)
        return NULL;
    bridge->server = server;
    bridge->listener_fd = -1;
    bridge->client_fd = -1;
    bridge->peer_state = PRESENTER_PEER_AWAIT_HELLO;
    int length = snprintf(bridge->socket_path, sizeof(bridge->socket_path),
            "%s/trierarch-presenter.sock", runtime_dir);
    if (length <= 0 || (size_t)length >= sizeof(bridge->socket_path))
        goto fail;
    bridge->listener_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC |
            SOCK_NONBLOCK, 0);
    if (bridge->listener_fd < 0)
        goto fail;
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (strlen(bridge->socket_path) >= sizeof(address.sun_path))
        goto fail;
    strcpy(address.sun_path, bridge->socket_path);
    unlink(bridge->socket_path);
    if (bind(bridge->listener_fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
            listen(bridge->listener_fd, 1) < 0)
        goto fail;
    chmod(bridge->socket_path, 0666);
    bridge->listener_source = wl_event_loop_add_fd(server->event_loop,
            bridge->listener_fd, WL_EVENT_READABLE | WL_EVENT_ERROR,
            listener_readable, bridge);
    if (!bridge->listener_source)
        goto fail;
    LOGI("presenter protocol listener ready: %s", bridge->socket_path);
    return bridge;

fail:
    LOGW("unable to create presenter protocol listener: %s", strerror(errno));
    trierarch_presenter_bridge_destroy(bridge);
    return NULL;
}

void trierarch_presenter_bridge_destroy(struct trierarch_presenter_bridge *bridge) {
    if (!bridge)
        return;
    if (bridge->listener_source)
        wl_event_source_remove(bridge->listener_source);
    close_client(bridge);
    if (bridge->listener_fd >= 0)
        close(bridge->listener_fd);
    if (bridge->socket_path[0])
        unlink(bridge->socket_path);
    free(bridge);
}
