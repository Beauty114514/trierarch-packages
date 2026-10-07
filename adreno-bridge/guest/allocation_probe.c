#define _POSIX_C_SOURCE 200809L

#include "trierarch_adreno_bridge_protocol.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define DRM_FORMAT_ABGR8888 0x34324241u

static int connect_bridge(const char *path) {
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
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

static int send_hello(int fd) {
    const struct trierarch_adreno_bridge_header hello = {
        .magic = TRIERARCH_ADRENO_BRIDGE_MAGIC,
        .version = TRIERARCH_ADRENO_BRIDGE_VERSION,
        .type = TRIERARCH_ADRENO_BRIDGE_HELLO,
    };
    return send(fd, &hello, sizeof(hello), MSG_NOSIGNAL) ==
            (ssize_t)sizeof(hello) ? 0 : -1;
}

static int send_allocate(int fd, uint32_t width, uint32_t height,
        uint32_t buffer_count) {
    const struct trierarch_adreno_bridge_allocate request = {
        .header = {
            .magic = TRIERARCH_ADRENO_BRIDGE_MAGIC,
            .version = TRIERARCH_ADRENO_BRIDGE_VERSION,
            .type = TRIERARCH_ADRENO_BRIDGE_ALLOCATE,
        },
        .width = width,
        .height = height,
        .drm_format = DRM_FORMAT_ABGR8888,
        .buffer_count = buffer_count,
    };
    return send(fd, &request, sizeof(request), MSG_NOSIGNAL) ==
            (ssize_t)sizeof(request) ? 0 : -1;
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
    int received_fd = -1;
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET ||
            cmsg->cmsg_type != SCM_RIGHTS ||
            cmsg->cmsg_len != CMSG_LEN(sizeof(received_fd)))
        return -1;
    memcpy(&received_fd, CMSG_DATA(cmsg), sizeof(received_fd));
    if (buffer->header.magic != TRIERARCH_ADRENO_BRIDGE_MAGIC ||
            buffer->header.version != TRIERARCH_ADRENO_BRIDGE_VERSION ||
            buffer->header.type != TRIERARCH_ADRENO_BRIDGE_BUFFER ||
            buffer->drm_format != DRM_FORMAT_ABGR8888 || !buffer->width ||
            !buffer->height || !buffer->stride || received_fd < 0) {
        if (received_fd >= 0)
            close(received_fd);
        errno = EPROTO;
        return -1;
    }
    return received_fd;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s SOCKET WIDTH HEIGHT BUFFER_COUNT\n", argv[0]);
        return EXIT_FAILURE;
    }
    char *end = NULL;
    unsigned long width = strtoul(argv[2], &end, 10);
    if (!end || *end || !width || width > UINT32_MAX)
        return EXIT_FAILURE;
    unsigned long height = strtoul(argv[3], &end, 10);
    if (!end || *end || !height || height > UINT32_MAX)
        return EXIT_FAILURE;
    unsigned long count = strtoul(argv[4], &end, 10);
    if (!end || *end || count < 2 || count > 4)
        return EXIT_FAILURE;

    int bridge_fd = connect_bridge(argv[1]);
    if (bridge_fd < 0 || send_hello(bridge_fd) < 0 ||
            send_allocate(bridge_fd, (uint32_t)width, (uint32_t)height,
                    (uint32_t)count) < 0) {
        perror("adreno-bridge-probe: request allocation");
        if (bridge_fd >= 0)
            close(bridge_fd);
        return EXIT_FAILURE;
    }

    for (unsigned long index = 0; index < count; ++index) {
        struct trierarch_adreno_bridge_buffer buffer = {0};
        int pixel_fd = receive_buffer(bridge_fd, &buffer);
        if (pixel_fd < 0) {
            perror("adreno-bridge-probe: receive allocation");
            close(bridge_fd);
            return EXIT_FAILURE;
        }
        struct stat status = {0};
        int stat_result = fstat(pixel_fd, &status);
        printf("adreno-bridge-probe: slot=%u %ux%u format=0x%x stride=%u "
                "device=%llu inode=%llu\n", buffer.buffer_id, buffer.width,
                buffer.height, buffer.drm_format, buffer.stride,
                (unsigned long long)status.st_dev,
                (unsigned long long)status.st_ino);
        close(pixel_fd);
        if (stat_result < 0) {
            perror("adreno-bridge-probe: fstat allocation");
            close(bridge_fd);
            return EXIT_FAILURE;
        }
    }
    close(bridge_fd);
    return EXIT_SUCCESS;
}
