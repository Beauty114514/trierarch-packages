#define _POSIX_C_SOURCE 200809L

#include "../protocol.h"

#include <gbm.h>

#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

enum { DEFAULT_WIDTH = 64, DEFAULT_HEIGHT = 64, MAX_DIMENSION = 4096 };

static int parse_dimension(const char *text, uint32_t *value) {
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 10);
    if (errno || !text[0] || (end && *end) || !parsed || parsed > MAX_DIMENSION)
        return -1;
    *value = (uint32_t)parsed;
    return 0;
}

static int fill_checkerboard(struct gbm_bo *bo, uint32_t width, uint32_t height) {
    uint32_t stride = 0;
    void *map_data = NULL;
    uint32_t *pixels = gbm_bo_map(bo, 0, 0, width, height,
            GBM_BO_TRANSFER_WRITE, &stride, &map_data);
    if (!pixels)
        return -1;
    uint32_t tile = (width < height ? width : height) / 8;
    if (!tile)
        tile = 1;
    for (uint32_t y = 0; y < height; ++y) {
        uint32_t *row = (uint32_t *)((uint8_t *)pixels + y * stride);
        for (uint32_t x = 0; x < width; ++x)
            row[x] = ((x / tile) + (y / tile)) & 1 ? 0x00ffffffu : 0x00000000u;
    }
    gbm_bo_unmap(bo, map_data);
    return 0;
}

static int connect_socket(const char *path) {
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(address.sun_path)) {
        close(fd);
        return -1;
    }
    strcpy(address.sun_path, path);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_message_with_fd(int socket_fd,
        const struct trierarch_gpu_probe_buffer *buffer, int buffer_fd) {
    char control[CMSG_SPACE(sizeof(buffer_fd))] = {0};
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
    cmsg->cmsg_len = CMSG_LEN(sizeof(buffer_fd));
    memcpy(CMSG_DATA(cmsg), &buffer_fd, sizeof(buffer_fd));
    return sendmsg(socket_fd, &message, 0) == (ssize_t)sizeof(*buffer) ? 0 : -1;
}

int main(int argc, char **argv) {
    if (argc != 2 && argc != 4 && argc != 5) {
        fprintf(stderr, "usage: %s /path/to/gpu-probe.sock [width height [require-ahb]]\n", argv[0]);
        return EXIT_FAILURE;
    }
    uint32_t width = DEFAULT_WIDTH;
    uint32_t height = DEFAULT_HEIGHT;
    if (argc >= 4 && (parse_dimension(argv[2], &width) || parse_dimension(argv[3], &height))) {
        fprintf(stderr, "guest-to-host: dimensions must be between 1 and %u\n", MAX_DIMENSION);
        return EXIT_FAILURE;
    }
    bool require_ahb = argc == 5 && !strcmp(argv[4], "require-ahb");
    if (argc == 5 && !require_ahb) {
        fputs("guest-to-host: optional mode must be require-ahb\n", stderr);
        return EXIT_FAILURE;
    }
    int status = EXIT_FAILURE;
    int render_fd = -1;
    int socket_fd = -1;
    int buffer_fd = -1;
    struct gbm_device *device = NULL;
    struct gbm_bo *bo = NULL;

    render_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    device = render_fd >= 0 ? gbm_create_device(render_fd) : NULL;
    bo = device ? gbm_bo_create(device, width, height,
            GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR) : NULL;
    buffer_fd = bo && fill_checkerboard(bo, width, height) == 0 ? gbm_bo_get_fd(bo) : -1;
    socket_fd = connect_socket(argv[1]);
    if (!bo || buffer_fd < 0 || socket_fd < 0) {
        fprintf(stderr, "guest-to-host: setup failed\n");
        goto out;
    }

    const struct trierarch_gpu_probe_hello hello = {
        .magic = TRIERARCH_GPU_PROBE_MAGIC,
        .version = TRIERARCH_GPU_PROBE_VERSION,
        .type = TRIERARCH_GPU_PROBE_HELLO,
        .direction = TRIERARCH_GPU_PROBE_GUEST_TO_HOST,
    };
    const struct trierarch_gpu_probe_buffer buffer = {
        .magic = TRIERARCH_GPU_PROBE_MAGIC,
        .version = TRIERARCH_GPU_PROBE_VERSION,
        .type = TRIERARCH_GPU_PROBE_GUEST_BUFFER,
        .width = gbm_bo_get_width(bo),
        .height = gbm_bo_get_height(bo),
        .drm_format = DRM_FORMAT_XRGB8888,
        .stride = gbm_bo_get_stride(bo),
        .modifier = gbm_bo_get_modifier(bo),
        .flags = require_ahb ? TRIERARCH_GPU_PROBE_BUFFER_REQUIRE_AHB : 0,
    };
    if (send(socket_fd, &hello, sizeof(hello), 0) != (ssize_t)sizeof(hello) ||
            send_message_with_fd(socket_fd, &buffer, buffer_fd) != 0) {
        fprintf(stderr, "guest-to-host: send failed\n");
        goto out;
    }
    struct trierarch_gpu_probe_result_message result = {0};
    if (recv(socket_fd, &result, sizeof(result), 0) != (ssize_t)sizeof(result) ||
            result.magic != TRIERARCH_GPU_PROBE_MAGIC ||
            result.version != TRIERARCH_GPU_PROBE_VERSION ||
            result.type != TRIERARCH_GPU_PROBE_RESULT) {
        fprintf(stderr, "guest-to-host: invalid host result\n");
        goto out;
    }
    printf("guest-to-host: %ux%u checkerboard result=%u egl=0x%x format=0x%x stride=%u modifier=0x%llx require-ahb=%d\n",
            buffer.width, buffer.height, result.result, result.egl_error, buffer.drm_format, buffer.stride,
            (unsigned long long)buffer.modifier, require_ahb);
    status = EXIT_SUCCESS;

out:
    if (socket_fd >= 0)
        close(socket_fd);
    if (buffer_fd >= 0)
        close(buffer_fd);
    if (bo)
        gbm_bo_destroy(bo);
    if (device)
        gbm_device_destroy(device);
    if (render_fd >= 0)
        close(render_fd);
    return status;
}
