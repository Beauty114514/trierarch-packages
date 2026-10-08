#define _GNU_SOURCE
#include "../protocol.h"
#include "vulkan_export.h"
#include "wayland_dmabuf_client.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int connect_socket(const char *path) {
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (fd < 0 || strlen(path) >= sizeof(address.sun_path)) return -1;
    strcpy(address.sun_path, path);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_hello(int fd) {
    const struct trierarch_gpu_probe_hello hello = {
        .magic = TRIERARCH_GPU_PROBE_MAGIC, .version = TRIERARCH_GPU_PROBE_VERSION,
        .type = TRIERARCH_GPU_PROBE_HELLO, .direction = TRIERARCH_GPU_PROBE_GUEST_TO_HOST,
    };
    return send(fd, &hello, sizeof(hello), MSG_NOSIGNAL) == (ssize_t)sizeof(hello) ? 0 : -1;
}

static int send_buffer(int socket_fd, const struct trierarch_gpu_probe_buffer *buffer, int fd) {
    char control[CMSG_SPACE(sizeof(fd))] = {0};
    struct iovec iov = { .iov_base = (void *)buffer, .iov_len = sizeof(*buffer) };
    struct msghdr message = { .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control, .msg_controllen = sizeof(control) };
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(fd));
    memcpy(CMSG_DATA(header), &fd, sizeof(fd));
    return sendmsg(socket_fd, &message, MSG_NOSIGNAL) == (ssize_t)sizeof(*buffer) ? 0 : -1;
}

static int send_to_probe_socket(const char *socket_path) {
    struct trierarch_guest_vulkan_image image = {0};
    int buffer_fd = -1;
    uint32_t stride = 0;
    uint64_t modifier = 0;
    if (trierarch_guest_create_exportable_image(&image, &buffer_fd, &stride, &modifier) < 0) {
        fputs("guest: unable to create exportable Turnip dma-buf\n", stderr);
        return 1;
    }
    int socket_fd = connect_socket(socket_path);
    struct trierarch_gpu_probe_buffer buffer = { .magic = TRIERARCH_GPU_PROBE_MAGIC,
        .version = TRIERARCH_GPU_PROBE_VERSION, .type = TRIERARCH_GPU_PROBE_GUEST_BUFFER,
        .width = 64, .height = 64, .drm_format = TRIERARCH_GUEST_DRM_FORMAT_ABGR8888,
        .stride = stride, .modifier = modifier };
    int status = socket_fd >= 0 && send_hello(socket_fd) == 0 &&
            send_buffer(socket_fd, &buffer, buffer_fd) == 0 ? 0 : 2;
    struct trierarch_gpu_probe_result_message result = {0};
    if (status == 0 && recv(socket_fd, &result, sizeof(result), 0) == (ssize_t)sizeof(result) &&
            result.magic == TRIERARCH_GPU_PROBE_MAGIC && result.result == TRIERARCH_GPU_PROBE_OK)
        printf("guest: host accepted dma-buf stride=%u modifier=0x%llx\n", stride,
                (unsigned long long)modifier);
    else if (status == 0) { fputs("guest: host rejected dma-buf\n", stderr); status = 3; }
    if (socket_fd >= 0) close(socket_fd);
    close(buffer_fd);
    trierarch_guest_destroy_exportable_image(&image);
    return status;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--wayland"))
        return trierarch_guest_submit_wayland_dmabuf();
    if (argc != 2) {
        fprintf(stderr, "usage: %s GPU_PROBE_SOCKET | %s --wayland\n", argv[0], argv[0]);
        return 64;
    }
    return send_to_probe_socket(argv[1]);
}
