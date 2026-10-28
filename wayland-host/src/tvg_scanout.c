#define _GNU_SOURCE
#include "tvg_scanout.h"

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef SYS_memfd_create
#define SYS_memfd_create 279
#endif

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

#define TAG "TrierarchTvg"
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

struct tvg_token {
    struct tvg_scanout_token info;
    struct tvg_token *next;
};

static pthread_mutex_t token_lock = PTHREAD_MUTEX_INITIALIZER;
static struct tvg_token *tokens;
static int listen_fd = -1;
static pthread_t server_thread;
static volatile bool running;
static char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];

static int new_memfd(const char *name) {
    return (int)syscall(SYS_memfd_create, name, MFD_CLOEXEC);
}

static uint32_t ahb_format_for_gbm(uint32_t gbm_format) {
    switch (gbm_format) {
    case 0x34324241: /* GBM_FORMAT_ABGR8888 / DRM_FORMAT_ABGR8888 */
    case 0x34325241: /* GBM_FORMAT_ARGB8888 */
        return AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    case 0x34324258: /* GBM_FORMAT_XBGR8888 */
    case 0x34325258: /* GBM_FORMAT_XRGB8888 */
        return AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM;
    default:
        return AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    }
}

static struct tvg_token *alloc_token(const struct tvg_request *request,
        int *out_fd, int32_t *status) {
    struct tvg_token *token = calloc(1, sizeof(*token));
    if (!token) {
        *status = -ENOMEM;
        return NULL;
    }

    uint32_t stride = request->stride ? request->stride : request->width * 4;
    AHardwareBuffer_Desc desc;
    memset(&desc, 0, sizeof(desc));
    desc.width = request->width;
    desc.height = request->height;
    desc.layers = 1;
    desc.format = ahb_format_for_gbm(request->format);
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                 AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER;

    AHardwareBuffer *ahb = NULL;
    int result = AHardwareBuffer_allocate(&desc, &ahb);
    if (result != 0) {
        desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                     AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
        result = AHardwareBuffer_allocate(&desc, &ahb);
    }
    if (result != 0 || !ahb) {
        __android_log_print(ANDROID_LOG_WARN, TAG,
                "AHardwareBuffer_allocate %ux%u failed: %d",
                request->width, request->height, result);
        free(token);
        *status = -EIO;
        return NULL;
    }

    int fd = new_memfd("tvg-token");
    if (fd < 0) {
        AHardwareBuffer_release(ahb);
        free(token);
        *status = -errno;
        return NULL;
    }
    if (ftruncate(fd, (off_t)stride * request->height) != 0) {
        close(fd);
        AHardwareBuffer_release(ahb);
        free(token);
        *status = -errno;
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        AHardwareBuffer_release(ahb);
        free(token);
        *status = -errno;
        return NULL;
    }

    token->info.fd = fd;
    token->info.device = st.st_dev;
    token->info.inode = st.st_ino;
    token->info.width = request->width;
    token->info.height = request->height;
    token->info.format = request->format;
    token->info.stride = stride;
    token->info.ahb = ahb;
    token->info.res_id = 0;

    *out_fd = fd;
    *status = 0;
    return token;
}

static bool send_reply(int client_fd, int token_fd,
        const struct tvg_alloc_reply *reply) {
    struct iovec iov = { .iov_base = (void *)reply, .iov_len = sizeof(*reply) };
    char control[CMSG_SPACE(sizeof(int))];
    memset(control, 0, sizeof(control));

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    if (token_fd >= 0) {
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);

        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &token_fd, sizeof(int));
    }

    ssize_t written;
    do {
        written = sendmsg(client_fd, &msg, MSG_NOSIGNAL);
    } while (written < 0 && errno == EINTR);
    return written == (ssize_t)sizeof(*reply);
}

#define TVG_PRESENT_MAGIC 0x54564750u

struct tvg_present_header {
    uint32_t magic;
    uint32_t ino;
    uint32_t res_id;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t stride;
};

typedef int (*send_handle_fn)(const AHardwareBuffer *buffer, int socket_fd);

static void send_present(const struct tvg_scanout_token *token) {
    if (!token || !token->ahb || token->res_id == 0)
        return;

    const char *path = getenv("TRIERARCH_TVG_PRESENT_SOCKET");
    if (!path || !*path)
        path = "/data/data/app.trierarch/files/virgl-run/present.sock";

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        static bool warned;
        if (!warned) {
            warned = true;
            __android_log_print(ANDROID_LOG_WARN, TAG,
                    "present socket %s unavailable: %s", path, strerror(errno));
        }
        close(fd);
        return;
    }

    struct tvg_present_header header;
    memset(&header, 0, sizeof(header));
    header.magic = TVG_PRESENT_MAGIC;
    header.ino = (uint32_t)token->inode;
    header.res_id = (uint32_t)token->res_id;
    header.width = token->width;
    header.height = token->height;
    header.format = token->format;
    header.stride = token->stride;

    if (send(fd, &header, sizeof(header), MSG_NOSIGNAL) == sizeof(header)) {
        static send_handle_fn send_handle;
        if (!send_handle) {
            void *library = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
            if (library)
                send_handle = (send_handle_fn)dlsym(library,
                        "AHardwareBuffer_sendHandleToUnixSocket");
        }
        if (send_handle && send_handle((const AHardwareBuffer *)token->ahb, fd) == 0) {
            struct timeval timeout = { .tv_sec = 1 };
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            int status = -1;
            if (recv(fd, &status, sizeof(status), MSG_WAITALL) == sizeof(status)) {
                __android_log_print(ANDROID_LOG_INFO, TAG,
                        "present res_id=%u blit status=%d",
                        (uint32_t)token->res_id, status);
            }
        }
    }
    close(fd);
}

static int register_token(uint64_t inode, uint32_t res_id) {
    int result = -1;
    struct tvg_scanout_token *registered = NULL;
    pthread_mutex_lock(&token_lock);
    for (struct tvg_token *token = tokens; token; token = token->next) {
        if (token->info.inode == (ino_t)inode) {
            token->info.res_id = (int)res_id;
            registered = &token->info;
            __android_log_print(ANDROID_LOG_INFO, TAG,
                    "token ino=%llu registered res_id=%u",
                    (unsigned long long)inode, res_id);
            result = 0;
            break;
        }
    }
    pthread_mutex_unlock(&token_lock);
    if (registered)
        send_present(registered);
    return result;
}

static void handle_client(int client_fd) {
    struct tvg_request request;
    ssize_t received;
    do {
        received = recv(client_fd, &request, sizeof(request), 0);
    } while (received < 0 && errno == EINTR);
    if (received != (ssize_t)sizeof(request) || request.magic != TVG_MAGIC) {
        return;
    }

    if (request.type == TVG_REQUEST_REGISTER) {
        struct tvg_alloc_reply reply;
        memset(&reply, 0, sizeof(reply));
        reply.magic = TVG_MAGIC;
        reply.status = register_token(request.inode, request.res_id);
        send_reply(client_fd, -1, &reply);
        return;
    }
    if (request.type != TVG_REQUEST_ALLOC)
        return;

    int token_fd = -1;
    int32_t status = 0;
    struct tvg_token *token = alloc_token(&request, &token_fd, &status);

    struct tvg_alloc_reply reply;
    memset(&reply, 0, sizeof(reply));
    reply.magic = TVG_MAGIC;
    reply.status = status;
    reply.stride = token ? token->info.stride : 0;
    reply.format = token ? token->info.format : 0;
    reply.inode = token ? (uint64_t)token->info.inode : 0;

    if (token) {
        __android_log_print(ANDROID_LOG_INFO, TAG,
                "token ino=%llu %ux%u fmt=0x%x stride=%u",
                (unsigned long long)token->info.inode,
                token->info.width, token->info.height,
                token->info.format, token->info.stride);
        pthread_mutex_lock(&token_lock);
        token->next = tokens;
        tokens = token;
        pthread_mutex_unlock(&token_lock);
    }

    send_reply(client_fd, token_fd, &reply);
}

static void *server_main(void *unused) {
    (void)unused;
    while (running) {
        struct pollfd pfd = { .fd = listen_fd, .events = POLLIN };
        int ready = poll(&pfd, 1, 200);
        if (ready <= 0)
            continue;
        int client_fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
        if (client_fd < 0)
            continue;
        handle_client(client_fd);
        close(client_fd);
    }
    return NULL;
}

int tvg_scanout_start(const char *path) {
    if (!path || strlen(path) >= sizeof(socket_path))
        return -EINVAL;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -errno;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    unlink(path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
            listen(fd, 4) != 0) {
        int result = -errno;
        close(fd);
        return result;
    }
    chmod(path, 0666);

    listen_fd = fd;
    snprintf(socket_path, sizeof(socket_path), "%s", path);
    running = true;
    if (pthread_create(&server_thread, NULL, server_main, NULL) != 0) {
        running = false;
        close(fd);
        listen_fd = -1;
        unlink(path);
        return -EIO;
    }
    __android_log_print(ANDROID_LOG_INFO, TAG, "listening on %s", path);
    return 0;
}

void tvg_scanout_stop(void) {
    if (!running)
        return;
    running = false;
    pthread_join(server_thread, NULL);
    if (listen_fd >= 0) {
        close(listen_fd);
        listen_fd = -1;
    }
    if (socket_path[0])
        unlink(socket_path);

    pthread_mutex_lock(&token_lock);
    struct tvg_token *token = tokens;
    tokens = NULL;
    pthread_mutex_unlock(&token_lock);
    while (token) {
        struct tvg_token *next = token->next;
        if (token->info.ahb)
            AHardwareBuffer_release(token->info.ahb);
        close(token->info.fd);
        free(token);
        token = next;
    }
}

const struct tvg_scanout_token *tvg_scanout_lookup_fd(int fd) {
    if (fd < 0)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0)
        return NULL;

    const struct tvg_scanout_token *found = NULL;
    pthread_mutex_lock(&token_lock);
    for (struct tvg_token *token = tokens; token; token = token->next) {
        if (token->info.device == st.st_dev && token->info.inode == st.st_ino) {
            found = &token->info;
            break;
        }
    }
    pthread_mutex_unlock(&token_lock);
    return found;
}

void tvg_scanout_present_fd(int fd) {
    const struct tvg_scanout_token *token = tvg_scanout_lookup_fd(fd);
    if (token)
        send_present(token);
}
