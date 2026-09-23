#define _GNU_SOURCE
#include "gpu_probe.h"
#include "server_internal.h"
#include "compositor.h"

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <dlfcn.h>
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

#define TAG "TrierarchGpuProbe"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define DRM_FORMAT_ABGR8888 0x34324241u
#define GPU_PROBE_BUFFER_COUNT 3u

/* VNDK-only: resolving this dynamically makes availability and native-handle
 * shape probe data instead of an unstated Android ABI dependency. */
struct native_handle { int version; int numFds; int numInts; int data[]; };
typedef const struct native_handle *(*get_native_handle_fn)(const AHardwareBuffer *);
enum client_mode { CLIENT_NONE, CLIENT_GUEST_TO_HOST, CLIENT_HOST_TO_GUEST };

struct host_buffer_slot {
    AHardwareBuffer *buffer;
    int guest_fence_fd;
    bool ready;
};

struct trierarch_gpu_probe {
    struct wayland_server *server;
    int listener_fd, client_fd, buffer_fd;
    struct wl_event_source *listener_source, *client_source;
    struct trierarch_gpu_probe_buffer buffer;
    struct host_buffer_slot host_buffers[GPU_PROBE_BUFFER_COUNT];
    char socket_path[PATH_MAX];
    enum client_mode mode;
    bool pending;
};

static void close_client(struct trierarch_gpu_probe *probe) {
    if (probe->client_source) { wl_event_source_remove(probe->client_source); probe->client_source = NULL; }
    if (probe->client_fd >= 0) close(probe->client_fd);
    probe->client_fd = -1; probe->mode = CLIENT_NONE;
}

static void discard_pending(struct trierarch_gpu_probe *probe) {
    if (probe->buffer_fd >= 0) close(probe->buffer_fd);
    probe->buffer_fd = -1;
    for (uint32_t index = 0; index < GPU_PROBE_BUFFER_COUNT; ++index) {
        if (probe->host_buffers[index].guest_fence_fd >= 0)
            close(probe->host_buffers[index].guest_fence_fd);
        probe->host_buffers[index].guest_fence_fd = -1;
        if (probe->host_buffers[index].buffer)
            AHardwareBuffer_release(probe->host_buffers[index].buffer);
        probe->host_buffers[index].buffer = NULL;
        probe->host_buffers[index].ready = false;
    }
    probe->pending = false;
    memset(&probe->buffer, 0, sizeof(probe->buffer));
}

static void send_result(int client_fd, uint32_t result, uint32_t egl_error,
        uint32_t buffer_id, int fence_fd) {
    const struct trierarch_gpu_probe_result_message message = {
        .magic = TRIERARCH_GPU_PROBE_MAGIC, .version = TRIERARCH_GPU_PROBE_VERSION,
        .type = TRIERARCH_GPU_PROBE_RESULT, .result = result, .egl_error = egl_error,
        .buffer_id = buffer_id,
    };
    if (fence_fd < 0) {
        (void)send(client_fd, &message, sizeof(message), MSG_NOSIGNAL);
        return;
    }
    char control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec iov = { .iov_base = (void *)&message, .iov_len = sizeof(message) };
    struct msghdr packet = { .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control, .msg_controllen = sizeof(control) };
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&packet);
    cmsg->cmsg_level = SOL_SOCKET; cmsg->cmsg_type = SCM_RIGHTS; cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fence_fd, sizeof(fence_fd));
    (void)sendmsg(client_fd, &packet, MSG_NOSIGNAL);
}

static int send_buffer(int client_fd, const struct trierarch_gpu_probe_buffer *buffer, int fd) {
    char control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec iov = { .iov_base = (void *)buffer, .iov_len = sizeof(*buffer) };
    struct msghdr message = { .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control, .msg_controllen = sizeof(control) };
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
    cmsg->cmsg_level = SOL_SOCKET; cmsg->cmsg_type = SCM_RIGHTS; cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
    return sendmsg(client_fd, &message, MSG_NOSIGNAL) == (ssize_t)sizeof(*buffer) ? 0 : -1;
}

static get_native_handle_fn get_native_handle(void) {
    static bool attempted;
    static get_native_handle_fn function;
    if (attempted) return function;
    attempted = true;
    function = (get_native_handle_fn)dlsym(RTLD_DEFAULT, "AHardwareBuffer_getNativeHandle");
    if (!function) {
        void *library = dlopen("libnativewindow.so", RTLD_NOW | RTLD_LOCAL);
        if (library) function = (get_native_handle_fn)dlsym(library, "AHardwareBuffer_getNativeHandle");
    }
    return function;
}

static int create_and_send_host_buffers(struct trierarch_gpu_probe *probe) {
    const AHardwareBuffer_Desc requested = {
        .width = 256, .height = 256, .layers = 1,
        .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
        .usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
    };
    get_native_handle_fn native_handle = get_native_handle();
    for (uint32_t index = 0; index < GPU_PROBE_BUFFER_COUNT; ++index) {
        struct host_buffer_slot *slot = &probe->host_buffers[index];
        if (AHardwareBuffer_allocate(&requested, &slot->buffer) != 0 || !slot->buffer) {
            LOGE("AHardwareBuffer_allocate failed for slot=%u", index); return -1;
        }
        AHardwareBuffer_Desc actual = {0}; AHardwareBuffer_describe(slot->buffer, &actual);
        const struct native_handle *handle = native_handle ? native_handle(slot->buffer) : NULL;
        if (!handle || handle->numFds < 1 || handle->data[0] < 0) {
            LOGE("Android AHardwareBuffer native handle unavailable for slot=%u", index); return -1;
        }
        LOGI("host AHardwareBuffer slot=%u: %ux%u stride=%u format=0x%x native_handle fds=%d ints=%d",
                index, actual.width, actual.height, actual.stride, actual.format,
                handle->numFds, handle->numInts);
        int fd = dup(handle->data[0]);
        if (fd < 0) return -1;
        struct trierarch_gpu_probe_buffer buffer = {
            .magic = TRIERARCH_GPU_PROBE_MAGIC, .version = TRIERARCH_GPU_PROBE_VERSION,
            .type = TRIERARCH_GPU_PROBE_HOST_BUFFER, .width = actual.width, .height = actual.height,
            /* R8G8B8A8 memory on little-endian Android is DRM ABGR8888. */
            .drm_format = DRM_FORMAT_ABGR8888, .stride = actual.stride * 4u,
            .modifier = 0, .buffer_id = index,
        };
        int result = send_buffer(probe->client_fd, &buffer, fd); close(fd);
        if (result < 0) return -1;
    }
    LOGI("sent %u Android AHardwareBuffer slots to guest; awaiting render results", GPU_PROBE_BUFFER_COUNT);
    return 0;
}

static int receive_hello(struct trierarch_gpu_probe *probe) {
    struct trierarch_gpu_probe_hello hello = {0};
    ssize_t received = recv(probe->client_fd, &hello, sizeof(hello), MSG_DONTWAIT);
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    if (received != (ssize_t)sizeof(hello) || hello.magic != TRIERARCH_GPU_PROBE_MAGIC ||
            hello.version != TRIERARCH_GPU_PROBE_VERSION || hello.type != TRIERARCH_GPU_PROBE_HELLO) return -1;
    if (hello.direction == TRIERARCH_GPU_PROBE_GUEST_TO_HOST) { probe->mode = CLIENT_GUEST_TO_HOST; return 1; }
    if (hello.direction != TRIERARCH_GPU_PROBE_HOST_TO_GUEST) return -1;
    probe->mode = CLIENT_HOST_TO_GUEST;
    return create_and_send_host_buffers(probe) == 0 ? 1 : -1;
}

static int receive_guest_buffer(struct trierarch_gpu_probe *probe) {
    char control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec iov = { .iov_base = &probe->buffer, .iov_len = sizeof(probe->buffer) };
    struct msghdr message = { .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control, .msg_controllen = sizeof(control) };
    ssize_t received = recvmsg(probe->client_fd, &message, MSG_DONTWAIT);
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    if (received != (ssize_t)sizeof(probe->buffer)) return -1;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS || cmsg->cmsg_len != CMSG_LEN(sizeof(int))) return -1;
    memcpy(&probe->buffer_fd, CMSG_DATA(cmsg), sizeof(probe->buffer_fd));
    if (probe->buffer.magic != TRIERARCH_GPU_PROBE_MAGIC || probe->buffer.version != TRIERARCH_GPU_PROBE_VERSION ||
            probe->buffer.type != TRIERARCH_GPU_PROBE_GUEST_BUFFER || !probe->buffer.width ||
            !probe->buffer.height || !probe->buffer.stride || probe->buffer_fd < 0) return -1;
    probe->pending = true;
    LOGI("received guest dma-buf: %ux%u format=0x%x stride=%u modifier=0x%llx", probe->buffer.width,
            probe->buffer.height, probe->buffer.drm_format, probe->buffer.stride,
            (unsigned long long)probe->buffer.modifier);
    return 1;
}

static int receive_guest_result(struct trierarch_gpu_probe *probe) {
    struct trierarch_gpu_probe_result_message result = {0};
    char control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec iov = { .iov_base = &result, .iov_len = sizeof(result) };
    struct msghdr packet = { .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control, .msg_controllen = sizeof(control) };
    ssize_t received = recvmsg(probe->client_fd, &packet, MSG_DONTWAIT);
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    if (received != (ssize_t)sizeof(result) || result.magic != TRIERARCH_GPU_PROBE_MAGIC ||
            result.version != TRIERARCH_GPU_PROBE_VERSION || result.type != TRIERARCH_GPU_PROBE_RESULT) return -1;
    if (result.result != TRIERARCH_GPU_PROBE_OK) {
        LOGE("guest could not import/render Android buffer: result=%u egl=0x%x", result.result, result.egl_error);
        send_result(probe->client_fd, result.result, result.egl_error, result.buffer_id, -1); return -1;
    }
    if (result.buffer_id >= GPU_PROBE_BUFFER_COUNT) {
        LOGE("guest result has invalid buffer slot=%u", result.buffer_id); return -1;
    }
    struct host_buffer_slot *slot = &probe->host_buffers[result.buffer_id];
    if (!slot->buffer || slot->ready || slot->guest_fence_fd >= 0) {
        LOGE("guest attempted to submit busy buffer slot=%u", result.buffer_id); return -1;
    }
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&packet);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
            cmsg->cmsg_len != CMSG_LEN(sizeof(int))) {
        LOGE("guest result lacks its release fence"); return -1;
    }
    memcpy(&slot->guest_fence_fd, CMSG_DATA(cmsg), sizeof(slot->guest_fence_fd));
    if (slot->guest_fence_fd < 0) return -1;
    slot->ready = true;
    probe->pending = true;
    LOGI("guest rendered Android buffer slot=%u; scheduling host sample", result.buffer_id);
    return 1;
}

static int client_readable(int fd, uint32_t mask, void *data) {
    (void)fd; struct trierarch_gpu_probe *probe = data;
    if (!probe || !(mask & (WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR))) return 0;
    int outcome = -1;
    if (mask & WL_EVENT_READABLE) {
        if (probe->mode == CLIENT_NONE) outcome = receive_hello(probe);
        else if (probe->mode == CLIENT_GUEST_TO_HOST) outcome = receive_guest_buffer(probe);
        else outcome = receive_guest_result(probe);
    }
    if (outcome == 1 && probe->pending) {
        if (probe->mode == CLIENT_GUEST_TO_HOST && probe->client_source) {
            wl_event_source_remove(probe->client_source); probe->client_source = NULL;
        }
        trierarch_wayland_request_render(probe->server); return 0;
    }
    if (outcome < 0 || (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))) {
        LOGE("guest gpu probe connection failed: %s", strerror(errno));
        if (probe->client_fd >= 0) send_result(probe->client_fd, TRIERARCH_GPU_PROBE_BAD_MESSAGE, 0, 0, -1);
        close_client(probe); discard_pending(probe);
    }
    return 0;
}

static int listener_readable(int fd, uint32_t mask, void *data) {
    (void)fd; struct trierarch_gpu_probe *probe = data;
    if (!probe || !(mask & WL_EVENT_READABLE) || probe->client_fd >= 0 || probe->pending) return 0;
    int client_fd = accept4(probe->listener_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (client_fd < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK) LOGE("accept failed: %s", strerror(errno)); return 0; }
    probe->client_fd = client_fd;
    probe->client_source = wl_event_loop_add_fd(probe->server->event_loop, client_fd,
            WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR, client_readable, probe);
    if (!probe->client_source) { LOGE("unable to watch guest probe client"); close_client(probe); }
    return 0;
}

struct trierarch_gpu_probe *trierarch_gpu_probe_create(struct wayland_server *server, const char *runtime_dir) {
    if (!server || !server->event_loop || !runtime_dir) return NULL;
    struct trierarch_gpu_probe *probe = calloc(1, sizeof(*probe)); if (!probe) return NULL;
    probe->server = server; probe->listener_fd = -1; probe->client_fd = -1; probe->buffer_fd = -1;
    for (uint32_t index = 0; index < GPU_PROBE_BUFFER_COUNT; ++index)
        probe->host_buffers[index].guest_fence_fd = -1;
    int length = snprintf(probe->socket_path, sizeof(probe->socket_path), "%s/gpu-probe.sock", runtime_dir);
    if (length <= 0 || (size_t)length >= sizeof(probe->socket_path)) goto fail;
    probe->listener_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0); if (probe->listener_fd < 0) goto fail;
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (strlen(probe->socket_path) >= sizeof(address.sun_path)) goto fail;
    strcpy(address.sun_path, probe->socket_path); unlink(probe->socket_path);
    if (bind(probe->listener_fd, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(probe->listener_fd, 1) < 0) goto fail;
    chmod(probe->socket_path, 0666);
    probe->listener_source = wl_event_loop_add_fd(server->event_loop, probe->listener_fd,
            WL_EVENT_READABLE | WL_EVENT_ERROR, listener_readable, probe);
    if (!probe->listener_source) goto fail;
    LOGI("test-only bidirectional dma-buf listener ready: %s", probe->socket_path); return probe;
fail:
    LOGE("unable to create gpu probe listener: %s", strerror(errno)); trierarch_gpu_probe_destroy(probe); return NULL;
}

void trierarch_gpu_probe_destroy(struct trierarch_gpu_probe *probe) {
    if (!probe) return;
    if (probe->listener_source) wl_event_source_remove(probe->listener_source);
    close_client(probe); discard_pending(probe);
    if (probe->listener_fd >= 0) close(probe->listener_fd);
    if (probe->socket_path[0]) unlink(probe->socket_path); free(probe);
}

bool trierarch_gpu_probe_take(struct trierarch_gpu_probe *probe, struct trierarch_gpu_probe_buffer *buffer,
        int *buffer_fd, int *client_fd) {
    if (!probe || !buffer || !buffer_fd || !client_fd || !probe->pending || probe->mode != CLIENT_GUEST_TO_HOST ||
            probe->buffer_fd < 0 || probe->client_fd < 0) return false;
    *buffer = probe->buffer; *buffer_fd = probe->buffer_fd; *client_fd = probe->client_fd;
    probe->buffer_fd = -1; probe->client_fd = -1; probe->pending = false; probe->mode = CLIENT_NONE;
    memset(&probe->buffer, 0, sizeof(probe->buffer)); return true;
}

bool trierarch_gpu_probe_take_host_buffer(struct trierarch_gpu_probe *probe, AHardwareBuffer **buffer,
        uint32_t *buffer_id, int *guest_fence_fd) {
    if (!probe || !buffer || !buffer_id || !guest_fence_fd || !probe->pending ||
            probe->mode != CLIENT_HOST_TO_GUEST || probe->client_fd < 0) return false;
    for (uint32_t index = 0; index < GPU_PROBE_BUFFER_COUNT; ++index) {
        struct host_buffer_slot *slot = &probe->host_buffers[index];
        if (!slot->ready || !slot->buffer || slot->guest_fence_fd < 0) continue;
        *buffer = slot->buffer;
        *buffer_id = index;
        *guest_fence_fd = slot->guest_fence_fd;
        slot->guest_fence_fd = -1;
        slot->ready = false;
        probe->pending = false;
        for (uint32_t other = 0; other < GPU_PROBE_BUFFER_COUNT; ++other)
            probe->pending = probe->pending || probe->host_buffers[other].ready;
        if (probe->pending)
            trierarch_wayland_request_render(probe->server);
        return true;
    }
    return false;
}

void trierarch_gpu_probe_report(int client_fd, uint32_t result, uint32_t egl_error, int fence_fd) {
    if (client_fd < 0) { if (fence_fd >= 0) close(fence_fd); return; }
    send_result(client_fd, result, egl_error, 0, fence_fd);
    if (fence_fd >= 0) close(fence_fd);
    close(client_fd);
}

void trierarch_gpu_probe_report_host_buffer(struct trierarch_gpu_probe *probe, uint32_t buffer_id,
        uint32_t result, uint32_t egl_error, int fence_fd) {
    if (!probe || probe->client_fd < 0 || buffer_id >= GPU_PROBE_BUFFER_COUNT) {
        if (fence_fd >= 0) close(fence_fd);
        return;
    }
    send_result(probe->client_fd, result, egl_error, buffer_id, fence_fd);
    if (fence_fd >= 0) close(fence_fd);
}
