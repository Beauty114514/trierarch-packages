#define _GNU_SOURCE
#include <gbm.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/*
 * LD_PRELOAD shim for the "last cell" POC.
 *
 * KWin allocates its swapchain with gbm_bo_create() and turns each bo's fd
 * into an EGLImage.  We do not have a guest DRM allocator that the host can
 * import, so ask the host token service for a memfd token instead.  The token
 * is only an identity: Mesa's vtest winsys creates a regular virgl texture
 * for the image and never reads the fd contents.
 */

#define TVG_MAGIC 0x54564731u

struct tvg_alloc_request {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t stride;
    uint32_t usage;
};

struct tvg_alloc_reply {
    uint32_t magic;
    int32_t status;
    uint32_t stride;
    uint32_t format;
    uint64_t inode;
};

struct gbm_device {
    int drm_fd;
    int tvg_fd;
};

struct gbm_bo {
    int token_fd;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint64_t modifier;
    void *user_data;
};

static int connect_tvg(void) {
    const char *path = getenv("TRIERARCH_TVG_SOCKET");
    if (!path || !*path) {
        fprintf(stderr, "gbm-shim: TRIERARCH_TVG_SOCKET is not set\n");
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "gbm-shim: connect(%s) failed: %s\n",
                path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int receive_reply(int fd, struct tvg_alloc_reply *reply) {
    struct iovec iov = { .iov_base = reply, .iov_len = sizeof(*reply) };
    char control[CMSG_SPACE(sizeof(int))];
    memset(control, 0, sizeof(control));

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    ssize_t received;
    do {
        received = recvmsg(fd, &msg, MSG_CMSG_CLOEXEC);
    } while (received < 0 && errno == EINTR);

    if (received != (ssize_t)sizeof(*reply))
        return -1;

    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            int token_fd;
            memcpy(&token_fd, CMSG_DATA(cmsg), sizeof(token_fd));
            return token_fd;
        }
    }
    return -1;
}

static struct gbm_bo *create_token_bo(struct gbm_device *gbm,
        uint32_t width, uint32_t height, uint32_t format, uint64_t modifier) {
    if (!gbm || width == 0 || height == 0)
        return NULL;
    if (gbm->tvg_fd < 0)
        gbm->tvg_fd = connect_tvg();
    if (gbm->tvg_fd < 0)
        return NULL;

    struct tvg_alloc_request request;
    memset(&request, 0, sizeof(request));
    request.magic = TVG_MAGIC;
    request.width = width;
    request.height = height;
    request.format = format;
    request.stride = width * 4;
    request.usage = GBM_BO_USE_RENDERING;

    if (send(gbm->tvg_fd, &request, sizeof(request), MSG_NOSIGNAL) !=
            (ssize_t)sizeof(request)) {
        fprintf(stderr, "gbm-shim: token request failed: %s\n", strerror(errno));
        return NULL;
    }

    struct tvg_alloc_reply reply;
    memset(&reply, 0, sizeof(reply));
    int token_fd = receive_reply(gbm->tvg_fd, &reply);
    if (token_fd < 0 || reply.magic != TVG_MAGIC || reply.status != 0) {
        fprintf(stderr, "gbm-shim: token allocation failed (status=%d)\n",
                reply.status);
        if (token_fd >= 0)
            close(token_fd);
        return NULL;
    }

    struct gbm_bo *bo = calloc(1, sizeof(*bo));
    if (!bo) {
        close(token_fd);
        return NULL;
    }
    bo->token_fd = token_fd;
    bo->width = width;
    bo->height = height;
    bo->stride = reply.stride ? reply.stride : width * 4;
    bo->format = format;
    bo->modifier = modifier;
    return bo;
}

struct gbm_device *gbm_create_device(int fd) {
    struct gbm_device *gbm = calloc(1, sizeof(*gbm));
    if (!gbm)
        return NULL;
    gbm->drm_fd = fd;
    gbm->tvg_fd = -1;
    return gbm;
}

void gbm_device_destroy(struct gbm_device *gbm) {
    if (!gbm)
        return;
    if (gbm->tvg_fd >= 0)
        close(gbm->tvg_fd);
    free(gbm);
}

int gbm_device_get_fd(struct gbm_device *gbm) {
    return gbm ? gbm->drm_fd : -1;
}

const char *gbm_device_get_backend_name(struct gbm_device *gbm) {
    (void)gbm;
    return "drm";
}

int gbm_device_is_format_supported(struct gbm_device *gbm, uint32_t format,
        uint32_t usage) {
    (void)gbm;
    (void)format;
    (void)usage;
    return 1;
}

struct gbm_bo *gbm_bo_create(struct gbm_device *gbm, uint32_t width,
        uint32_t height, uint32_t format, uint32_t flags) {
    (void)flags;
    return create_token_bo(gbm, width, height, format, 0);
}

struct gbm_bo *gbm_bo_create_with_modifiers(struct gbm_device *gbm,
        uint32_t width, uint32_t height, uint32_t format,
        const uint64_t *modifiers, const unsigned int count) {
    uint64_t modifier = (modifiers && count > 0) ? modifiers[0] : 0;
    return create_token_bo(gbm, width, height, format, modifier);
}

void gbm_bo_destroy(struct gbm_bo *bo) {
    if (!bo)
        return;
    if (bo->token_fd >= 0)
        close(bo->token_fd);
    free(bo);
}

int gbm_bo_get_fd(struct gbm_bo *bo) {
    return bo ? dup(bo->token_fd) : -1;
}

int gbm_bo_get_fd_for_plane(struct gbm_bo *bo, int plane) {
    return plane == 0 ? gbm_bo_get_fd(bo) : -1;
}

uint32_t gbm_bo_get_stride(struct gbm_bo *bo) {
    return bo ? bo->stride : 0;
}

uint32_t gbm_bo_get_format(struct gbm_bo *bo) {
    return bo ? bo->format : 0;
}

uint64_t gbm_bo_get_modifier(struct gbm_bo *bo) {
    return bo ? bo->modifier : 0;
}

uint32_t gbm_bo_get_width(struct gbm_bo *bo) {
    return bo ? bo->width : 0;
}

uint32_t gbm_bo_get_height(struct gbm_bo *bo) {
    return bo ? bo->height : 0;
}

int gbm_bo_get_plane_count(struct gbm_bo *bo) {
    (void)bo;
    return 1;
}

uint32_t gbm_bo_get_offset(struct gbm_bo *bo, int plane) {
    (void)bo;
    (void)plane;
    return 0;
}

int gbm_bo_get_plane_fd(struct gbm_bo *bo, int plane) {
    return gbm_bo_get_fd_for_plane(bo, plane);
}

union gbm_bo_handle gbm_bo_get_handle(struct gbm_bo *bo) {
    union gbm_bo_handle handle;
    memset(&handle, 0, sizeof(handle));
    (void)bo;
    return handle;
}

union gbm_bo_handle gbm_bo_get_handle_for_plane(struct gbm_bo *bo, int plane) {
    (void)plane;
    return gbm_bo_get_handle(bo);
}

int gbm_bo_get_plane_count_stride(struct gbm_bo *bo, int plane,
        uint32_t *stride) {
    if (!bo || plane != 0 || !stride)
        return -EINVAL;
    *stride = bo->stride;
    return 0;
}

void *gbm_bo_map(struct gbm_bo *bo, uint32_t x, uint32_t y, uint32_t width,
        uint32_t height, uint32_t flags, uint32_t *stride, void **map_data) {
    (void)bo; (void)x; (void)y; (void)width; (void)height;
    (void)flags; (void)stride; (void)map_data;
    errno = EINVAL;
    return NULL;
}

void gbm_bo_unmap(struct gbm_bo *bo, void *map_data) {
    (void)bo;
    (void)map_data;
}

void gbm_bo_set_user_data(struct gbm_bo *bo, void *data,
        void (*destroy_user_data)(struct gbm_bo *, void *)) {
    (void)destroy_user_data;
    if (bo)
        bo->user_data = data;
}

void *gbm_bo_get_user_data(struct gbm_bo *bo) {
    return bo ? bo->user_data : NULL;
}
