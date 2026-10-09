#include "dmabuf_feedback_device.h"
#include "server_internal.h"
#include "dmabuf_device_report_protocol.h"

#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <unistd.h>

#define TAG "TrierarchWayland"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

struct trierarch_dmabuf_feedback_device {
    struct wayland_server *server;
    struct wl_event_source *listener_source;
    struct wl_event_source *client_source;
    int listener_fd;
    int client_fd;
    char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    dev_t device;
    bool known;
};

static void close_client(struct trierarch_dmabuf_feedback_device *device) {
    if (device->client_source) {
        wl_event_source_remove(device->client_source);
        device->client_source = NULL;
    }
    if (device->client_fd >= 0) {
        close(device->client_fd);
        device->client_fd = -1;
    }
}

static int client_readable(int fd, uint32_t mask, void *data) {
    (void)fd;
    struct trierarch_dmabuf_feedback_device *device = data;
    if (!device) return 0;
    if (!(mask & WL_EVENT_READABLE)) {
        close_client(device);
        return 0;
    }
    struct trierarch_dmabuf_device_report report = {0};
    ssize_t received = recv(device->client_fd, &report, sizeof(report), MSG_DONTWAIT);
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    close_client(device);
    if (received != (ssize_t)sizeof(report) ||
            report.magic != TRIERARCH_DMABUF_DEVICE_REPORT_MAGIC ||
            report.version != TRIERARCH_DMABUF_DEVICE_REPORT_VERSION) {
        LOGW("discarded malformed dma-buf device report");
        return 0;
    }
    device->device = makedev(report.major, report.minor);
    device->known = true;
    LOGI("guest dma-buf main-device reported as %u:%u", report.major, report.minor);
    return 0;
}

static int listener_readable(int fd, uint32_t mask, void *data) {
    (void)fd;
    struct trierarch_dmabuf_feedback_device *device = data;
    if (!device || !(mask & WL_EVENT_READABLE) || device->client_fd >= 0) return 0;
    int client = accept4(device->listener_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (client < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            LOGW("accept dma-buf device report: %s", strerror(errno));
        return 0;
    }
    device->client_fd = client;
    device->client_source = wl_event_loop_add_fd(device->server->event_loop, client,
            WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR, client_readable, device);
    if (!device->client_source) {
        LOGW("unable to watch dma-buf device-report client");
        close_client(device);
    }
    return 0;
}

struct trierarch_dmabuf_feedback_device *trierarch_dmabuf_feedback_device_create(
        struct wayland_server *server, const char *runtime_dir) {
    if (!server || !server->event_loop || !runtime_dir) return NULL;
    struct trierarch_dmabuf_feedback_device *device = calloc(1, sizeof(*device));
    if (!device) return NULL;
    device->server = server;
    device->listener_fd = -1;
    device->client_fd = -1;
    int length = snprintf(device->socket_path, sizeof(device->socket_path),
            "%s/dmabuf-feedback.sock", runtime_dir);
    if (length <= 0 || (size_t)length >= sizeof(device->socket_path)) goto fail;
    device->listener_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (device->listener_fd < 0) goto fail;
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    strcpy(address.sun_path, device->socket_path);
    unlink(device->socket_path);
    if (bind(device->listener_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
            listen(device->listener_fd, 4) != 0) goto fail;
    chmod(device->socket_path, 0666);
    device->listener_source = wl_event_loop_add_fd(server->event_loop, device->listener_fd,
            WL_EVENT_READABLE | WL_EVENT_ERROR, listener_readable, device);
    if (!device->listener_source) goto fail;
    LOGI("dma-buf feedback device-report listener ready: %s", device->socket_path);
    return device;
fail:
    if (device->listener_fd >= 0) close(device->listener_fd);
    if (device->socket_path[0]) unlink(device->socket_path);
    free(device);
    return NULL;
}

void trierarch_dmabuf_feedback_device_destroy(struct trierarch_dmabuf_feedback_device *device) {
    if (!device) return;
    close_client(device);
    if (device->listener_source) wl_event_source_remove(device->listener_source);
    if (device->listener_fd >= 0) close(device->listener_fd);
    if (device->socket_path[0]) unlink(device->socket_path);
    free(device);
}

bool trierarch_dmabuf_feedback_device_get(const struct trierarch_dmabuf_feedback_device *device,
        dev_t *result) {
    if (!device || !device->known || !result) return false;
    *result = device->device;
    return true;
}
