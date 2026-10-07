#define _POSIX_C_SOURCE 200809L

#include "trierarch_adreno_bridge_client.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int send_exact(int fd, const void *message, size_t size) {
    return send(fd, message, size, MSG_NOSIGNAL) == (ssize_t)size ? 0 : -1;
}

static int receive_buffer(int fd, struct trierarch_adreno_bridge_buffer *buffer) {
    char control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec iov = { .iov_base = buffer, .iov_len = sizeof(*buffer) };
    struct msghdr message = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = control,
        .msg_controllen = sizeof(control),
    };
    if (recvmsg(fd, &message, 0) != (ssize_t)sizeof(*buffer))
        return -1;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
    int pixel_fd = -1;
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET ||
            cmsg->cmsg_type != SCM_RIGHTS ||
            cmsg->cmsg_len != CMSG_LEN(sizeof(pixel_fd))) {
        errno = EPROTO;
        return -1;
    }
    memcpy(&pixel_fd, CMSG_DATA(cmsg), sizeof(pixel_fd));
    if (buffer->header.magic != TRIERARCH_ADRENO_BRIDGE_MAGIC ||
            buffer->header.version != TRIERARCH_ADRENO_BRIDGE_VERSION ||
            buffer->header.type != TRIERARCH_ADRENO_BRIDGE_BUFFER ||
            !buffer->width || !buffer->height || !buffer->stride ||
            pixel_fd < 0) {
        if (pixel_fd >= 0)
            close(pixel_fd);
        errno = EPROTO;
        return -1;
    }
    return pixel_fd;
}

void trierarch_adreno_bridge_client_close(
        struct trierarch_adreno_bridge_client *client) {
    if (!client)
        return;
    for (uint32_t index = 0; index < client->buffer_count; ++index) {
        if (client->pixel_fds[index] >= 0)
            close(client->pixel_fds[index]);
        client->pixel_fds[index] = -1;
    }
    if (client->socket_fd >= 0)
        close(client->socket_fd);
    memset(client, 0, sizeof(*client));
    client->socket_fd = -1;
    for (uint32_t index = 0; index < TRIERARCH_ADRENO_BRIDGE_MAX_BUFFERS;
            ++index)
        client->pixel_fds[index] = -1;
}

int trierarch_adreno_bridge_client_open(
        struct trierarch_adreno_bridge_client *client, const char *socket_path) {
    if (!client || !socket_path || !socket_path[0]) {
        errno = EINVAL;
        return -1;
    }
    memset(client, 0, sizeof(*client));
    client->socket_fd = -1;
    for (uint32_t index = 0; index < TRIERARCH_ADRENO_BRIDGE_MAX_BUFFERS;
            ++index)
        client->pixel_fds[index] = -1;
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (strlen(socket_path) >= sizeof(address.sun_path)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(address.sun_path, socket_path);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        return -1;
    }
    const struct trierarch_adreno_bridge_header hello = {
        .magic = TRIERARCH_ADRENO_BRIDGE_MAGIC,
        .version = TRIERARCH_ADRENO_BRIDGE_VERSION,
        .type = TRIERARCH_ADRENO_BRIDGE_HELLO,
    };
    if (send_exact(fd, &hello, sizeof(hello)) < 0) {
        close(fd);
        return -1;
    }
    client->socket_fd = fd;
    return 0;
}

int trierarch_adreno_bridge_client_allocate(
        struct trierarch_adreno_bridge_client *client, uint32_t width,
        uint32_t height, uint32_t drm_format, uint32_t buffer_count) {
    if (!client || client->socket_fd < 0 || !width || !height ||
            buffer_count < 2 || buffer_count > TRIERARCH_ADRENO_BRIDGE_MAX_BUFFERS) {
        errno = EINVAL;
        return -1;
    }
    const struct trierarch_adreno_bridge_allocate request = {
        .header = {
            .magic = TRIERARCH_ADRENO_BRIDGE_MAGIC,
            .version = TRIERARCH_ADRENO_BRIDGE_VERSION,
            .type = TRIERARCH_ADRENO_BRIDGE_ALLOCATE,
        },
        .width = width,
        .height = height,
        .drm_format = drm_format,
        .buffer_count = buffer_count,
    };
    if (send_exact(client->socket_fd, &request, sizeof(request)) < 0)
        return -1;
    for (uint32_t index = 0; index < buffer_count; ++index) {
        int pixel_fd = receive_buffer(client->socket_fd, &client->buffers[index]);
        if (pixel_fd < 0)
            goto fail;
        client->pixel_fds[index] = pixel_fd;
        client->buffer_count = index + 1;
    }
    return 0;

fail:
    for (uint32_t index = 0; index < client->buffer_count; ++index) {
        close(client->pixel_fds[index]);
        client->pixel_fds[index] = -1;
    }
    client->buffer_count = 0;
    return -1;
}

const struct trierarch_adreno_bridge_buffer *trierarch_adreno_bridge_client_buffer(
        const struct trierarch_adreno_bridge_client *client, uint32_t index,
        int *pixel_fd) {
    if (!client || index >= client->buffer_count ||
            client->pixel_fds[index] < 0)
        return NULL;
    if (pixel_fd)
        *pixel_fd = client->pixel_fds[index];
    return &client->buffers[index];
}

int trierarch_adreno_bridge_client_release(
        struct trierarch_adreno_bridge_client *client, uint32_t buffer_id) {
    if (!client || client->socket_fd < 0 || buffer_id >= client->buffer_count) {
        errno = EINVAL;
        return -1;
    }
    const struct trierarch_adreno_bridge_release release = {
        .header = {
            .magic = TRIERARCH_ADRENO_BRIDGE_MAGIC,
            .version = TRIERARCH_ADRENO_BRIDGE_VERSION,
            .type = TRIERARCH_ADRENO_BRIDGE_RELEASE,
        },
        .buffer_id = buffer_id,
    };
    return send_exact(client->socket_fd, &release, sizeof(release));
}
