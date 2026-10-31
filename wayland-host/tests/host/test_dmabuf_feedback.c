#include "compositor.h"
#include "dmabuf_device_report_protocol.h"

#include <assert.h>
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
static volatile int server_should_stop;
static pthread_t server_thread_id;

static void *server_thread(void *unused) {
    (void)unused;
    while (!server_should_stop) trierarch_wayland_dispatch(server);
    return NULL;
}

static void start_server(const char *runtime_dir) {
    server_should_stop = 0;
    server = trierarch_wayland_create(runtime_dir, true);
    assert(server != NULL);
    assert(pthread_create(&server_thread_id, NULL, server_thread, NULL) == 0);
}

static void stop_server(void) {
    server_should_stop = 1;
    trierarch_wayland_wake(server);
    pthread_join(server_thread_id, NULL);
    trierarch_wayland_destroy(server);
    server = NULL;
}

static int connect_report_socket(const char *runtime_dir) {
    char path[4096];
    int length = snprintf(path, sizeof(path), "%s/dmabuf-feedback.sock", runtime_dir);
    assert(length > 0 && (size_t)length < sizeof(path));

    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    assert(fd >= 0);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    assert(strlen(path) < sizeof(address.sun_path));
    strcpy(address.sun_path, path);
    assert(connect(fd, (const struct sockaddr *)&address, sizeof(address)) == 0);
    return fd;
}

static uint32_t report_valid_device(const char *runtime_dir) {
    int fd = connect_report_socket(runtime_dir);
    struct trierarch_dmabuf_device_report report = {
        .magic = TRIERARCH_DMABUF_DEVICE_REPORT_MAGIC,
        .version = TRIERARCH_DMABUF_DEVICE_REPORT_VERSION,
        .major = 1,
        .minor = 1,
    };
    assert(send(fd, &report, sizeof(report), MSG_NOSIGNAL) == (ssize_t)sizeof(report));
    struct trierarch_dmabuf_device_report_ack ack = {0};
    assert(recv(fd, &ack, sizeof(ack), 0) == (ssize_t)sizeof(ack));
    close(fd);
    assert(ack.magic == TRIERARCH_DMABUF_DEVICE_REPORT_MAGIC);
    assert(ack.version == TRIERARCH_DMABUF_DEVICE_REPORT_VERSION);
    return ack.status;
}

static uint32_t report_truncated_device(const char *runtime_dir) {
    int fd = connect_report_socket(runtime_dir);
    uint32_t magic = TRIERARCH_DMABUF_DEVICE_REPORT_MAGIC;
    assert(send(fd, &magic, sizeof(magic), MSG_NOSIGNAL) == (ssize_t)sizeof(magic));
    struct trierarch_dmabuf_device_report_ack ack = {0};
    assert(recv(fd, &ack, sizeof(ack), 0) == (ssize_t)sizeof(ack));
    close(fd);
    assert(ack.magic == TRIERARCH_DMABUF_DEVICE_REPORT_MAGIC);
    assert(ack.version == TRIERARCH_DMABUF_DEVICE_REPORT_VERSION);
    return ack.status;
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

static int global_is_advertised(void) {
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

static char *make_runtime_dir(char *template) {
    char *runtime_dir = mkdtemp(template);
    assert(runtime_dir != NULL);
    return runtime_dir;
}

int main(void) {
    char first_template[] = "/tmp/trierarch-wayland-dmabuf-XXXXXX";
    char *first_runtime = make_runtime_dir(first_template);
    start_server(first_runtime);

    /* No report yet: the feedback socket exists but the global must not. */
    assert(!global_is_advertised());
    assert(report_valid_device(first_runtime) ==
            TRIERARCH_DMABUF_DEVICE_REPORT_STATUS_ACCEPTED);
    assert(global_is_advertised());
    stop_server();
    rmdir(first_runtime);

    /* A truncated report must be rejected and must not publish the global. */
    char second_template[] = "/tmp/trierarch-wayland-dmabuf-invalid-XXXXXX";
    char *second_runtime = make_runtime_dir(second_template);
    start_server(second_runtime);
    assert(report_truncated_device(second_runtime) ==
            TRIERARCH_DMABUF_DEVICE_REPORT_STATUS_INVALID);
    assert(!global_is_advertised());
    stop_server();
    rmdir(second_runtime);

    puts("wayland dmabuf feedback test passed");
    return 0;
}
