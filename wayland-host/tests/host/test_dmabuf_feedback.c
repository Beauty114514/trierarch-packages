#include "compositor.h"
#include "dmabuf_device_report_protocol.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-client.h>

static struct wayland_server *server;
static volatile int stop_server;

static void *server_thread(void *unused) {
    (void)unused;
    while (!stop_server) trierarch_wayland_dispatch(server);
    return NULL;
}

static int report_render_device(const char *runtime_dir) {
    char path[4096];
    int length = snprintf(path, sizeof(path), "%s/dmabuf-feedback.sock", runtime_dir);
    assert(length > 0 && (size_t)length < sizeof(path));

    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    assert(fd >= 0);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    assert(strlen(path) < sizeof(address.sun_path));
    strcpy(address.sun_path, path);
    assert(connect(fd, (const struct sockaddr *)&address, sizeof(address)) == 0);

    struct trierarch_dmabuf_device_report report = {
        .magic = TRIERARCH_DMABUF_DEVICE_REPORT_MAGIC,
        .version = TRIERARCH_DMABUF_DEVICE_REPORT_VERSION,
        .major = 1,
        .minor = 1,
    };
    assert(send(fd, &report, sizeof(report), MSG_NOSIGNAL) == (ssize_t)sizeof(report));

    struct trierarch_dmabuf_device_report_ack ack = {0};
    assert(recv(fd, &ack, sizeof(ack), 0) == (ssize_t)sizeof(ack));
    assert(ack.magic == TRIERARCH_DMABUF_DEVICE_REPORT_MAGIC);
    assert(ack.version == TRIERARCH_DMABUF_DEVICE_REPORT_VERSION);
    assert(ack.status == TRIERARCH_DMABUF_DEVICE_REPORT_STATUS_ACCEPTED);
    close(fd);
    return 0;
}

struct registry_state {
    int saw_linux_dmabuf;
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
        const char *interface, uint32_t version) {
    (void)registry; (void)name; (void)version;
    struct registry_state *state = data;
    if (strcmp(interface, "zwp_linux_dmabuf_v1") == 0) state->saw_linux_dmabuf = 1;
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static int global_is_advertised(const char *runtime_dir) {
    (void)runtime_dir;
    struct wl_display *display = wl_display_connect("wayland-trierarch");
    assert(display != NULL);
    struct registry_state state = {0};
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, &state);
    assert(wl_display_roundtrip(display) >= 0);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return state.saw_linux_dmabuf;
}

int main(void) {
    char template[] = "/tmp/trierarch-wayland-dmabuf-XXXXXX";
    char *runtime_dir = mkdtemp(template);
    assert(runtime_dir != NULL);

    server = trierarch_wayland_create(runtime_dir, true);
    assert(server != NULL);
    {
        char socket_path[4096];
        int length = snprintf(socket_path, sizeof(socket_path), "%s/dmabuf-feedback.sock", runtime_dir);
        assert(length > 0 && (size_t)length < sizeof(socket_path));
        assert(access(socket_path, F_OK) == 0);
    }

    pthread_t thread;
    assert(pthread_create(&thread, NULL, server_thread, NULL) == 0);
    report_render_device(runtime_dir);
    assert(global_is_advertised(runtime_dir));

    stop_server = 1;
    trierarch_wayland_wake(server);
    pthread_join(thread, NULL);
    trierarch_wayland_destroy(server);
    rmdir(runtime_dir);
    puts("wayland dmabuf feedback test passed");
    return 0;
}
