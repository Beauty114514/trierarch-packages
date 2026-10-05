#define _POSIX_C_SOURCE 200809L

#include "host_ahb_channel.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int trierarch_host_ahb_connect(const char *path) {
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(address.sun_path)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(address.sun_path, path);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int trierarch_host_ahb_request_buffers(int fd) {
    const struct trierarch_gpu_probe_hello hello = {
        .magic = TRIERARCH_GPU_PROBE_MAGIC,
        .version = TRIERARCH_GPU_PROBE_VERSION,
        .type = TRIERARCH_GPU_PROBE_HELLO,
        .direction = TRIERARCH_GPU_PROBE_HOST_TO_GUEST,
    };
    return send(fd, &hello, sizeof(hello), 0) == (ssize_t)sizeof(hello) ? 0 : -1;
}

int trierarch_host_ahb_receive_buffer(int fd,
        struct trierarch_gpu_probe_buffer *buffer) {
    char control[CMSG_SPACE(sizeof(int))] = {0};
    int received_fd = -1;
    struct iovec iov = { .iov_base = buffer, .iov_len = sizeof(*buffer) };
    struct msghdr packet = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = control,
        .msg_controllen = sizeof(control),
    };
    if (recvmsg(fd, &packet, 0) != (ssize_t)sizeof(*buffer)) return -1;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&packet);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
            cmsg->cmsg_len != CMSG_LEN(sizeof(received_fd))) return -1;
    memcpy(&received_fd, CMSG_DATA(cmsg), sizeof(received_fd));
    if (buffer->magic != TRIERARCH_GPU_PROBE_MAGIC ||
            buffer->version != TRIERARCH_GPU_PROBE_VERSION ||
            buffer->type != TRIERARCH_GPU_PROBE_HOST_BUFFER ||
            !buffer->width || !buffer->height || !buffer->stride || received_fd < 0) {
        if (received_fd >= 0) close(received_fd);
        return -1;
    }
    return received_fd;
}
