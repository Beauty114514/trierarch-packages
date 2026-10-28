#define _GNU_SOURCE
#include <gbm.h>

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
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
 * Only gbm_bo_create* is redirected, and only for callers outside Mesa.  KWin
 * allocates its swapchain with gbm_bo_create() and turns each bo's fd into an
 * EGLImage; we hand it a host memfd token instead.  Mesa's own GBM calls (EGL
 * internal surfaces/buffers) must keep using the real libgbm, otherwise they
 * receive a token that is not a real GBM bo and crash during eglInitialize.
 */

#define TVG_MAGIC 0x54564731u
#define TVG_REQUEST_ALLOC 0u
#define TVG_REQUEST_REGISTER 1u

struct tvg_request {
    uint32_t magic;
    uint32_t type;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t stride;
    uint32_t usage;
    uint64_t inode;
    uint32_t res_id;
};

struct tvg_alloc_reply {
    uint32_t magic;
    int32_t status;
    uint32_t stride;
    uint32_t format;
    uint64_t inode;
};

struct gbm_bo {
    int token_fd;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint64_t modifier;
    void *user_data;
    struct gbm_bo *next;
};

static pthread_mutex_t token_lock = PTHREAD_MUTEX_INITIALIZER;
static struct gbm_bo *token_bos;
static int tvg_fd = -1;

static bool is_token_bo(struct gbm_bo *bo) {
    if (!bo)
        return false;
    bool found = false;
    pthread_mutex_lock(&token_lock);
    for (struct gbm_bo *it = token_bos; it; it = it->next) {
        if (it == bo) {
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&token_lock);
    return found;
}

static bool addr_is_mesa(void *addr) {
    Dl_info info;
    if (!dladdr(addr, &info) || !info.dli_fname)
        return false;
    const char *name = info.dli_fname;
    return strstr(name, "libEGL") || strstr(name, "libgallium") ||
           strstr(name, "libgbm") || strstr(name, "libdril") ||
           strstr(name, "mesa");
}

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
    (void)gbm;
    if (width == 0 || height == 0)
        return NULL;

    pthread_mutex_lock(&token_lock);
    if (tvg_fd < 0)
        tvg_fd = connect_tvg();
    int fd = tvg_fd;
    pthread_mutex_unlock(&token_lock);
    if (fd < 0)
        return NULL;

    struct tvg_request request;
    memset(&request, 0, sizeof(request));
    request.magic = TVG_MAGIC;
    request.type = TVG_REQUEST_ALLOC;
    request.width = width;
    request.height = height;
    request.format = format;
    request.stride = width * 4;
    request.usage = GBM_BO_USE_RENDERING;

    if (send(fd, &request, sizeof(request), MSG_NOSIGNAL) !=
            (ssize_t)sizeof(request)) {
        fprintf(stderr, "gbm-shim: token request failed: %s\n", strerror(errno));
        return NULL;
    }

    struct tvg_alloc_reply reply;
    memset(&reply, 0, sizeof(reply));
    int token_fd = receive_reply(fd, &reply);
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

    pthread_mutex_lock(&token_lock);
    bo->next = token_bos;
    token_bos = bo;
    pthread_mutex_unlock(&token_lock);
    return bo;
}

#define REAL(name) \
    static typeof(name) *real_##name; \
    if (!real_##name) real_##name = (typeof(name) *)dlsym(RTLD_NEXT, #name)

struct gbm_bo *gbm_bo_create(struct gbm_device *gbm, uint32_t width,
        uint32_t height, uint32_t format, uint32_t flags) {
    void *caller = __builtin_return_address(0);
    if (addr_is_mesa(caller)) {
        REAL(gbm_bo_create);
        return real_gbm_bo_create
                ? real_gbm_bo_create(gbm, width, height, format, flags)
                : NULL;
    }
    (void)flags;
    return create_token_bo(gbm, width, height, format, 0);
}

struct gbm_bo *gbm_bo_create_with_modifiers(struct gbm_device *gbm,
        uint32_t width, uint32_t height, uint32_t format,
        const uint64_t *modifiers, const unsigned int count) {
    if (addr_is_mesa(__builtin_return_address(0))) {
        REAL(gbm_bo_create_with_modifiers);
        return real_gbm_bo_create_with_modifiers
                ? real_gbm_bo_create_with_modifiers(gbm, width, height, format,
                        modifiers, count)
                : NULL;
    }
    uint64_t modifier = (modifiers && count > 0) ? modifiers[0] : 0;
    return create_token_bo(gbm, width, height, format, modifier);
}

struct gbm_bo *gbm_bo_create_with_modifiers2(struct gbm_device *gbm,
        uint32_t width, uint32_t height, uint32_t format,
        const uint64_t *modifiers, const unsigned int count, uint32_t flags) {
    if (addr_is_mesa(__builtin_return_address(0))) {
        REAL(gbm_bo_create_with_modifiers2);
        return real_gbm_bo_create_with_modifiers2
                ? real_gbm_bo_create_with_modifiers2(gbm, width, height, format,
                        modifiers, count, flags)
                : NULL;
    }
    (void)flags;
    uint64_t modifier = (modifiers && count > 0) ? modifiers[0] : 0;
    return create_token_bo(gbm, width, height, format, modifier);
}

void gbm_bo_destroy(struct gbm_bo *bo) {
    if (!is_token_bo(bo)) {
        REAL(gbm_bo_destroy);
        if (real_gbm_bo_destroy)
            real_gbm_bo_destroy(bo);
        return;
    }

    pthread_mutex_lock(&token_lock);
    for (struct gbm_bo **it = &token_bos; *it; it = &(*it)->next) {
        if (*it == bo) {
            *it = bo->next;
            break;
        }
    }
    pthread_mutex_unlock(&token_lock);
    if (bo->token_fd >= 0)
        close(bo->token_fd);
    free(bo);
}

int gbm_bo_get_fd(struct gbm_bo *bo) {
    if (is_token_bo(bo))
        return bo->token_fd >= 0 ? dup(bo->token_fd) : -1;
    REAL(gbm_bo_get_fd);
    return real_gbm_bo_get_fd ? real_gbm_bo_get_fd(bo) : -1;
}

int gbm_bo_get_fd_for_plane(struct gbm_bo *bo, int plane) {
    if (is_token_bo(bo))
        return plane == 0 ? gbm_bo_get_fd(bo) : -1;
    REAL(gbm_bo_get_fd_for_plane);
    return real_gbm_bo_get_fd_for_plane
            ? real_gbm_bo_get_fd_for_plane(bo, plane)
            : -1;
}

uint32_t gbm_bo_get_stride(struct gbm_bo *bo) {
    if (is_token_bo(bo))
        return bo->stride;
    REAL(gbm_bo_get_stride);
    return real_gbm_bo_get_stride ? real_gbm_bo_get_stride(bo) : 0;
}

uint32_t gbm_bo_get_stride_for_plane(struct gbm_bo *bo, int plane) {
    if (is_token_bo(bo))
        return plane == 0 ? bo->stride : 0;
    REAL(gbm_bo_get_stride_for_plane);
    return real_gbm_bo_get_stride_for_plane
            ? real_gbm_bo_get_stride_for_plane(bo, plane)
            : 0;
}

uint32_t gbm_bo_get_format(struct gbm_bo *bo) {
    if (is_token_bo(bo))
        return bo->format;
    REAL(gbm_bo_get_format);
    return real_gbm_bo_get_format ? real_gbm_bo_get_format(bo) : 0;
}

uint64_t gbm_bo_get_modifier(struct gbm_bo *bo) {
    if (is_token_bo(bo))
        return bo->modifier;
    REAL(gbm_bo_get_modifier);
    return real_gbm_bo_get_modifier ? real_gbm_bo_get_modifier(bo) : 0;
}

uint32_t gbm_bo_get_width(struct gbm_bo *bo) {
    if (is_token_bo(bo))
        return bo->width;
    REAL(gbm_bo_get_width);
    return real_gbm_bo_get_width ? real_gbm_bo_get_width(bo) : 0;
}

uint32_t gbm_bo_get_height(struct gbm_bo *bo) {
    if (is_token_bo(bo))
        return bo->height;
    REAL(gbm_bo_get_height);
    return real_gbm_bo_get_height ? real_gbm_bo_get_height(bo) : 0;
}

uint32_t gbm_bo_get_bpp(struct gbm_bo *bo) {
    if (is_token_bo(bo))
        return 32;
    REAL(gbm_bo_get_bpp);
    return real_gbm_bo_get_bpp ? real_gbm_bo_get_bpp(bo) : 0;
}

int gbm_bo_get_plane_count(struct gbm_bo *bo) {
    if (is_token_bo(bo))
        return 1;
    REAL(gbm_bo_get_plane_count);
    return real_gbm_bo_get_plane_count ? real_gbm_bo_get_plane_count(bo) : 1;
}

uint32_t gbm_bo_get_offset(struct gbm_bo *bo, int plane) {
    if (is_token_bo(bo))
        return 0;
    REAL(gbm_bo_get_offset);
    return real_gbm_bo_get_offset ? real_gbm_bo_get_offset(bo, plane) : 0;
}

int gbm_bo_get_plane_fd(struct gbm_bo *bo, int plane) {
    if (is_token_bo(bo))
        return gbm_bo_get_fd_for_plane(bo, plane);
    REAL(gbm_bo_get_plane_fd);
    return real_gbm_bo_get_plane_fd ? real_gbm_bo_get_plane_fd(bo, plane) : -1;
}

union gbm_bo_handle gbm_bo_get_handle(struct gbm_bo *bo) {
    if (!is_token_bo(bo)) {
        REAL(gbm_bo_get_handle);
        if (real_gbm_bo_get_handle)
            return real_gbm_bo_get_handle(bo);
    }
    union gbm_bo_handle handle;
    memset(&handle, 0, sizeof(handle));
    return handle;
}

union gbm_bo_handle gbm_bo_get_handle_for_plane(struct gbm_bo *bo, int plane) {
    if (!is_token_bo(bo)) {
        REAL(gbm_bo_get_handle_for_plane);
        if (real_gbm_bo_get_handle_for_plane)
            return real_gbm_bo_get_handle_for_plane(bo, plane);
    }
    union gbm_bo_handle handle;
    memset(&handle, 0, sizeof(handle));
    return handle;
}

void *gbm_bo_map(struct gbm_bo *bo, uint32_t x, uint32_t y, uint32_t width,
        uint32_t height, uint32_t flags, uint32_t *stride, void **map_data) {
    if (!is_token_bo(bo)) {
        REAL(gbm_bo_map);
        return real_gbm_bo_map
                ? real_gbm_bo_map(bo, x, y, width, height, flags, stride, map_data)
                : NULL;
    }
    (void)bo; (void)x; (void)y; (void)width; (void)height;
    (void)flags; (void)stride; (void)map_data;
    errno = EINVAL;
    return NULL;
}

void gbm_bo_unmap(struct gbm_bo *bo, void *map_data) {
    if (!is_token_bo(bo)) {
        REAL(gbm_bo_unmap);
        if (real_gbm_bo_unmap)
            real_gbm_bo_unmap(bo, map_data);
    }
}

void gbm_bo_set_user_data(struct gbm_bo *bo, void *data,
        void (*destroy_user_data)(struct gbm_bo *, void *)) {
    if (!is_token_bo(bo)) {
        REAL(gbm_bo_set_user_data);
        if (real_gbm_bo_set_user_data)
            real_gbm_bo_set_user_data(bo, data, destroy_user_data);
        return;
    }
    (void)destroy_user_data;
    bo->user_data = data;
}

void *gbm_bo_get_user_data(struct gbm_bo *bo) {
    if (is_token_bo(bo))
        return bo->user_data;
    REAL(gbm_bo_get_user_data);
    return real_gbm_bo_get_user_data ? real_gbm_bo_get_user_data(bo) : NULL;
}
