#define _GNU_SOURCE
#include "gpu_probe.h"
#include "server_internal.h"
#include "compositor.h"
#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <android/log.h>

#define TAG "TrierarchGpuProbe"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

struct trierarch_gpu_probe {
    struct wayland_server *server;
    int listener_fd;
    int client_fd;
    int buffer_fd;
    struct wl_event_source *listener_source;
    struct wl_event_source *client_source;
    struct trierarch_gpu_probe_buffer buffer;
    char socket_path[PATH_MAX];
    bool pending;
};

static void close_client(struct trierarch_gpu_probe *probe) {
    if (!probe) return;
    if (probe->client_source) {
        wl_event_source_remove(probe->client_source);
        probe->client_source = NULL;
    }
    if (probe->client_fd >= 0) close(probe->client_fd);
    probe->client_fd = -1;
}

static void discard_pending(struct trierarch_gpu_probe *probe) {
    if (!probe) return;
    if (probe->buffer_fd >= 0) close(probe->buffer_fd);
    probe->buffer_fd = -1;
    probe->pending = false;
    memset(&probe->buffer, 0, sizeof(probe->buffer));
}

static void send_result(int client_fd, uint32_t result, uint32_t egl_error) {
    const struct trierarch_gpu_probe_result_message message = {
        .magic = TRIERARCH_GPU_PROBE_MAGIC,
        .version = TRIERARCH_GPU_PROBE_VERSION,
        .type = TRIERARCH_GPU_PROBE_RESULT,
        .result = result,
        .egl_error = egl_error,
    };
    (void)send(client_fd, &message, sizeof(message), MSG_NOSIGNAL);
}

static int receive_buffer(struct trierarch_gpu_probe *probe) {
    char control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec iov = { .iov_base = &probe->buffer, .iov_len = sizeof(probe->buffer) };
    struct msghdr message = { .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control, .msg_controllen = sizeof(control) };
    ssize_t received = recvmsg(probe->client_fd, &message, MSG_DONTWAIT);
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    if (received != (ssize_t)sizeof(probe->buffer)) return -1;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
            cmsg->cmsg_len != CMSG_LEN(sizeof(int))) return -1;
    memcpy(&probe->buffer_fd, CMSG_DATA(cmsg), sizeof(probe->buffer_fd));
    if (probe->buffer.magic != TRIERARCH_GPU_PROBE_MAGIC ||
            probe->buffer.version != TRIERARCH_GPU_PROBE_VERSION ||
            probe->buffer.type != TRIERARCH_GPU_PROBE_BUFFER ||
            !probe->buffer.width || !probe->buffer.height || !probe->buffer.stride ||
            probe->buffer_fd < 0) return -1;
    probe->pending = true;
    LOGI("received guest dma-buf: %ux%u format=0x%x stride=%u modifier=0x%llx",
            probe->buffer.width, probe->buffer.height, probe->buffer.drm_format,
            probe->buffer.stride, (unsigned long long)probe->buffer.modifier);
    return 1;
}

static int client_readable(int fd, uint32_t mask, void *data) {
    (void)fd;
    struct trierarch_gpu_probe *probe = data;
    if (!probe || !(mask & (WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR))) return 0;
    int outcome = (mask & WL_EVENT_READABLE) ? receive_buffer(probe) : -1;
    if (outcome == 1) {
        if (probe->client_source) {
            wl_event_source_remove(probe->client_source);
            probe->client_source = NULL;
        }
        trierarch_wayland_request_render(probe->server);
        return 0;
    }
    if (outcome < 0) {
        LOGE("guest probe sent an invalid buffer: %s", strerror(errno));
        if (probe->client_fd >= 0) send_result(probe->client_fd,
                TRIERARCH_GPU_PROBE_BAD_MESSAGE, 0);
        close_client(probe);
        discard_pending(probe);
    }
    return 0;
}

static int listener_readable(int fd, uint32_t mask, void *data) {
    (void)fd;
    struct trierarch_gpu_probe *probe = data;
    if (!probe || !(mask & WL_EVENT_READABLE) || probe->client_fd >= 0 || probe->pending) return 0;
    int client_fd = accept4(probe->listener_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (client_fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) LOGE("accept failed: %s", strerror(errno));
        return 0;
    }
    probe->client_fd = client_fd;
    probe->client_source = wl_event_loop_add_fd(probe->server->event_loop, client_fd,
            WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR, client_readable, probe);
    if (!probe->client_source) {
        LOGE("unable to watch guest probe client");
        close_client(probe);
    }
    return 0;
}

struct trierarch_gpu_probe *trierarch_gpu_probe_create(struct wayland_server *server,
        const char *runtime_dir) {
    if (!server || !server->event_loop || !runtime_dir) return NULL;
    struct trierarch_gpu_probe *probe = calloc(1, sizeof(*probe));
    if (!probe) return NULL;
    probe->server = server;
    probe->listener_fd = -1;
    probe->client_fd = -1;
    probe->buffer_fd = -1;
    int length = snprintf(probe->socket_path, sizeof(probe->socket_path),
            "%s/gpu-probe.sock", runtime_dir);
    if (length <= 0 || (size_t)length >= sizeof(probe->socket_path)) goto fail;
    probe->listener_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (probe->listener_fd < 0) goto fail;
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (strlen(probe->socket_path) >= sizeof(address.sun_path)) goto fail;
    strcpy(address.sun_path, probe->socket_path);
    unlink(probe->socket_path);
    if (bind(probe->listener_fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
            listen(probe->listener_fd, 1) < 0) goto fail;
    chmod(probe->socket_path, 0666);
    probe->listener_source = wl_event_loop_add_fd(server->event_loop, probe->listener_fd,
            WL_EVENT_READABLE | WL_EVENT_ERROR, listener_readable, probe);
    if (!probe->listener_source) goto fail;
    LOGI("test-only guest dma-buf listener ready: %s", probe->socket_path);
    return probe;
fail:
    LOGE("unable to create guest dma-buf probe listener: %s", strerror(errno));
    trierarch_gpu_probe_destroy(probe);
    return NULL;
}

void trierarch_gpu_probe_destroy(struct trierarch_gpu_probe *probe) {
    if (!probe) return;
    if (probe->listener_source) wl_event_source_remove(probe->listener_source);
    close_client(probe);
    discard_pending(probe);
    if (probe->listener_fd >= 0) close(probe->listener_fd);
    if (probe->socket_path[0]) unlink(probe->socket_path);
    free(probe);
}

bool trierarch_gpu_probe_take(struct trierarch_gpu_probe *probe,
        struct trierarch_gpu_probe_buffer *buffer, int *buffer_fd, int *client_fd) {
    if (!probe || !buffer || !buffer_fd || !client_fd || !probe->pending ||
            probe->buffer_fd < 0 || probe->client_fd < 0) return false;
    *buffer = probe->buffer;
    *buffer_fd = probe->buffer_fd;
    *client_fd = probe->client_fd;
    probe->buffer_fd = -1;
    probe->client_fd = -1;
    probe->pending = false;
    memset(&probe->buffer, 0, sizeof(probe->buffer));
    return true;
}

void trierarch_gpu_probe_report(int client_fd, uint32_t result, uint32_t egl_error) {
    if (client_fd < 0) return;
    send_result(client_fd, result, egl_error);
    close(client_fd);
}
