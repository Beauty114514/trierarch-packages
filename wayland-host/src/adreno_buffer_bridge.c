#define _GNU_SOURCE
#include "adreno_buffer_bridge.h"

#include "adreno_ahb_native.h"
#include "server_internal.h"
#include "trierarch_adreno_bridge_protocol.h"

#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define TAG "TrierarchAdrenoBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define DRM_FORMAT_ABGR8888 0x34324241u
#define MAX_BRIDGE_BUFFERS 4u
#define MAX_BRIDGE_DIMENSION 8192u

struct bridge_slot {
    AHardwareBuffer *buffer;
    dev_t device;
    ino_t inode;
    uint32_t id;
};

struct trierarch_adreno_buffer_bridge {
    struct wayland_server *server;
    int listener_fd;
    int client_fd;
    struct wl_event_source *listener_source;
    struct wl_event_source *client_source;
    struct bridge_slot slots[MAX_BRIDGE_BUFFERS];
    uint32_t slot_count;
    char socket_path[PATH_MAX];
};

static const struct trierarch_adreno_bridge_header bridge_hello = {
    .magic = TRIERARCH_ADRENO_BRIDGE_MAGIC,
    .version = TRIERARCH_ADRENO_BRIDGE_VERSION,
    .type = TRIERARCH_ADRENO_BRIDGE_HELLO,
};

static bool valid_header(const struct trierarch_adreno_bridge_header *header,
        uint16_t expected_type) {
    return header && header->magic == TRIERARCH_ADRENO_BRIDGE_MAGIC &&
            header->version == TRIERARCH_ADRENO_BRIDGE_VERSION &&
            header->type == expected_type;
}

static void clear_slots(struct trierarch_adreno_buffer_bridge *bridge) {
    for (uint32_t index = 0; index < bridge->slot_count; ++index) {
        if (bridge->slots[index].buffer)
            AHardwareBuffer_release(bridge->slots[index].buffer);
        memset(&bridge->slots[index], 0, sizeof(bridge->slots[index]));
    }
    bridge->slot_count = 0;
}

static void close_client(struct trierarch_adreno_buffer_bridge *bridge) {
    if (bridge->client_source) {
        wl_event_source_remove(bridge->client_source);
        bridge->client_source = NULL;
    }
    if (bridge->client_fd >= 0)
        close(bridge->client_fd);
    bridge->client_fd = -1;
    clear_slots(bridge);
}

static void send_error(struct trierarch_adreno_buffer_bridge *bridge,
        uint32_t code) {
    const struct trierarch_adreno_bridge_error error = {
        .header = {
            .magic = TRIERARCH_ADRENO_BRIDGE_MAGIC,
            .version = TRIERARCH_ADRENO_BRIDGE_VERSION,
            .type = TRIERARCH_ADRENO_BRIDGE_ERROR,
        },
        .code = code,
    };
    if (bridge->client_fd >= 0)
        (void)send(bridge->client_fd, &error, sizeof(error), MSG_NOSIGNAL);
}

static int send_buffer(struct trierarch_adreno_buffer_bridge *bridge,
        const struct trierarch_adreno_bridge_buffer *buffer, int fd) {
    char control[CMSG_SPACE(sizeof(fd))] = {0};
    struct iovec iov = { .iov_base = (void *)buffer, .iov_len = sizeof(*buffer) };
    struct msghdr message = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = control,
        .msg_controllen = sizeof(control),
    };
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(fd));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
    return sendmsg(bridge->client_fd, &message, MSG_NOSIGNAL) ==
            (ssize_t)sizeof(*buffer) ? 0 : -1;
}

static int allocate_pool(struct trierarch_adreno_buffer_bridge *bridge,
        const struct trierarch_adreno_bridge_allocate *request) {
    if (!valid_header(&request->header, TRIERARCH_ADRENO_BRIDGE_ALLOCATE) ||
            request->drm_format != DRM_FORMAT_ABGR8888 || !request->width ||
            !request->height || request->width > MAX_BRIDGE_DIMENSION ||
            request->height > MAX_BRIDGE_DIMENSION || request->buffer_count < 2 ||
            request->buffer_count > MAX_BRIDGE_BUFFERS) {
        return -1;
    }

    clear_slots(bridge);
    for (uint32_t index = 0; index < request->buffer_count; ++index) {
        struct bridge_slot *slot = &bridge->slots[index];
        const AHardwareBuffer_Desc requested = {
            .width = request->width,
            .height = request->height,
            .layers = 1,
            .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
            .usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                    AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
        };
        if (AHardwareBuffer_allocate(&requested, &slot->buffer) != 0 ||
                !slot->buffer)
            goto fail;

        AHardwareBuffer_Desc actual = {0};
        AHardwareBuffer_describe(slot->buffer, &actual);
        const struct trierarch_adreno_native_handle *handle =
                trierarch_adreno_ahb_native_handle(slot->buffer);
        if (!handle || handle->num_fds < 1 || handle->data[0] < 0)
            goto fail;

        int fd = dup(handle->data[0]);
        if (fd < 0)
            goto fail;
        struct stat statbuf;
        if (fstat(fd, &statbuf) < 0) {
            close(fd);
            goto fail;
        }
        slot->device = statbuf.st_dev;
        slot->inode = statbuf.st_ino;
        slot->id = index;
        bridge->slot_count = index + 1;
        const struct trierarch_adreno_bridge_buffer response = {
            .header = {
                .magic = TRIERARCH_ADRENO_BRIDGE_MAGIC,
                .version = TRIERARCH_ADRENO_BRIDGE_VERSION,
                .type = TRIERARCH_ADRENO_BRIDGE_BUFFER,
            },
            .buffer_id = index,
            .width = actual.width,
            .height = actual.height,
            .drm_format = DRM_FORMAT_ABGR8888,
            .stride = actual.stride * 4u,
            .modifier = 0,
        };
        int result = send_buffer(bridge, &response, fd);
        close(fd);
        if (result < 0)
            goto fail;
    }
    LOGI("allocated %u host-owned buffers: %ux%u format=ABGR8888",
            bridge->slot_count, request->width, request->height);
    return 0;

fail:
    clear_slots(bridge);
    return -1;
}

static int client_readable(int fd, uint32_t mask, void *data) {
    (void)fd;
    struct trierarch_adreno_buffer_bridge *bridge = data;
    if (!bridge)
        return 0;
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        close_client(bridge);
        return 0;
    }
    if (!(mask & WL_EVENT_READABLE))
        return 0;

    union {
        struct trierarch_adreno_bridge_header header;
        struct trierarch_adreno_bridge_allocate allocate;
        struct trierarch_adreno_bridge_release release;
    } message = {0};
    ssize_t received = recv(bridge->client_fd, &message, sizeof(message),
            MSG_DONTWAIT);
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return 0;
    if (received == (ssize_t)sizeof(bridge_hello) &&
            valid_header(&message.header, TRIERARCH_ADRENO_BRIDGE_HELLO))
        return 0;
    if (received == (ssize_t)sizeof(message.allocate) &&
            valid_header(&message.allocate.header,
                    TRIERARCH_ADRENO_BRIDGE_ALLOCATE)) {
        if (allocate_pool(bridge, &message.allocate) == 0)
            return 0;
        LOGE("rejected host buffer allocation request");
        send_error(bridge, EINVAL);
    } else if (received == (ssize_t)sizeof(message.release) &&
            valid_header(&message.release.header,
                    TRIERARCH_ADRENO_BRIDGE_RELEASE)) {
        /* The host owns the pool. A release is currently an integrity check;
         * the next rendering step will use it to return a slot to Mesa only
         * after Android presentation has retired. */
        if (message.release.buffer_id >= bridge->slot_count)
            send_error(bridge, EINVAL);
        return 0;
    } else {
        LOGE("invalid local bridge message");
        send_error(bridge, EPROTO);
    }
    close_client(bridge);
    return 0;
}

static int listener_readable(int fd, uint32_t mask, void *data) {
    (void)fd;
    struct trierarch_adreno_buffer_bridge *bridge = data;
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

struct trierarch_adreno_buffer_bridge *trierarch_adreno_buffer_bridge_create(
        struct wayland_server *server, const char *runtime_dir) {
    if (!server || !server->event_loop || !runtime_dir)
        return NULL;
    struct trierarch_adreno_buffer_bridge *bridge = calloc(1, sizeof(*bridge));
    if (!bridge)
        return NULL;
    bridge->server = server;
    bridge->listener_fd = -1;
    bridge->client_fd = -1;
    int length = snprintf(bridge->socket_path, sizeof(bridge->socket_path),
            "%s/adreno-buffer-bridge.sock", runtime_dir);
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
    LOGI("local host-buffer bridge ready: %s", bridge->socket_path);
    return bridge;

fail:
    LOGE("unable to create local host-buffer bridge: %s", strerror(errno));
    trierarch_adreno_buffer_bridge_destroy(bridge);
    return NULL;
}

void trierarch_adreno_buffer_bridge_destroy(
        struct trierarch_adreno_buffer_bridge *bridge) {
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

AHardwareBuffer *trierarch_adreno_buffer_bridge_lookup(
        const struct trierarch_adreno_buffer_bridge *bridge, int dmabuf_fd,
        uint32_t *buffer_id) {
    if (!bridge || dmabuf_fd < 0)
        return NULL;
    struct stat statbuf;
    if (fstat(dmabuf_fd, &statbuf) < 0)
        return NULL;
    for (uint32_t index = 0; index < bridge->slot_count; ++index) {
        const struct bridge_slot *slot = &bridge->slots[index];
        if (slot->buffer && slot->device == statbuf.st_dev &&
                slot->inode == statbuf.st_ino) {
            if (buffer_id)
                *buffer_id = slot->id;
            return slot->buffer;
        }
    }
    return NULL;
}
