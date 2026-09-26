#define _POSIX_C_SOURCE 200809L

#include <wayland-client.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { BUFFER_WIDTH = 320, BUFFER_HEIGHT = 180, BYTES_PER_PIXEL = 4 };

struct globals {
    struct wl_compositor *compositor;
    struct wl_shm *shm;
};

static void registry_global(void *data, struct wl_registry *registry,
        uint32_t name, const char *interface, uint32_t version) {
    struct globals *globals = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        globals->compositor = wl_registry_bind(registry, name,
                &wl_compositor_interface, version < 4 ? version : 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        globals->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
        uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void sleep_ms(long milliseconds) {
    struct timespec delay = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000000L,
    };
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

static int create_shm_fd(size_t size) {
    char path[] = "/tmp/trierarch-surface-probe-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
        return -1;
    unlink(path);
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static struct wl_buffer *create_solid_buffer(struct wl_shm *shm, uint32_t color) {
    const int stride = BUFFER_WIDTH * BYTES_PER_PIXEL;
    const size_t size = (size_t)stride * BUFFER_HEIGHT;
    int fd = create_shm_fd(size);
    if (fd < 0)
        return NULL;
    uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    for (size_t index = 0; index < (size_t)BUFFER_WIDTH * BUFFER_HEIGHT; index++)
        pixels[index] = color;
    munmap(pixels, size);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int)size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0,
            BUFFER_WIDTH, BUFFER_HEIGHT, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

static int commit_buffer(struct wl_display *display, struct wl_surface *surface,
        struct wl_buffer *buffer, const char *state) {
    fprintf(stdout, "%s\n", state);
    wl_surface_attach(surface, buffer, 0, 0);
    if (buffer)
        wl_surface_damage_buffer(surface, 0, 0, BUFFER_WIDTH, BUFFER_HEIGHT);
    wl_surface_commit(surface);
    return wl_display_flush(display) < 0 && errno != EAGAIN ? -1 : 0;
}

static int parse_hold_ms(int argc, char **argv, long *hold_ms) {
    if (argc == 1)
        return 0;
    if (argc != 3 || strcmp(argv[1], "--hold-ms") != 0)
        return -1;
    char *end = NULL;
    long value = strtol(argv[2], &end, 10);
    if (!end || *end != '\0' || value < 1 || value > 10000)
        return -1;
    *hold_ms = value;
    return 0;
}

int main(int argc, char **argv) {
    long hold_ms = 1000;
    if (parse_hold_ms(argc, argv, &hold_ms) != 0) {
        fprintf(stderr, "Usage: %s [--hold-ms 1..10000]\n", argv[0]);
        return EXIT_FAILURE;
    }
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "Unable to connect to WAYLAND_DISPLAY.\n");
        return EXIT_FAILURE;
    }
    struct globals globals = {0};
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, &globals);
    if (wl_display_roundtrip(display) < 0 || !globals.compositor || !globals.shm) {
        fprintf(stderr, "Compositor does not provide wl_compositor and wl_shm.\n");
        wl_registry_destroy(registry);
        wl_display_disconnect(display);
        return EXIT_FAILURE;
    }
    struct wl_surface *surface = wl_compositor_create_surface(globals.compositor);
    struct wl_buffer *red = create_solid_buffer(globals.shm, 0x00ff0000u);
    struct wl_buffer *green = create_solid_buffer(globals.shm, 0x0000ff00u);
    if (!surface || !red || !green) {
        fprintf(stderr, "Unable to allocate test surface buffers.\n");
        if (green) wl_buffer_destroy(green);
        if (red) wl_buffer_destroy(red);
        if (surface) wl_surface_destroy(surface);
        wl_shm_destroy(globals.shm);
        wl_compositor_destroy(globals.compositor);
        wl_registry_destroy(registry);
        wl_display_disconnect(display);
        return EXIT_FAILURE;
    }
    int status = 0;
    status |= commit_buffer(display, surface, red, "probe: attached red SHM buffer");
    sleep_ms(hold_ms);
    status |= commit_buffer(display, surface, NULL, "probe: committed attach(NULL)");
    sleep_ms(hold_ms);
    status |= commit_buffer(display, surface, green, "probe: attached green SHM buffer");
    sleep_ms(hold_ms);
    wl_buffer_destroy(green);
    wl_buffer_destroy(red);
    wl_surface_destroy(surface);
    wl_shm_destroy(globals.shm);
    wl_compositor_destroy(globals.compositor);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    if (status != 0) {
        fprintf(stderr, "Wayland display flush failed.\n");
        return EXIT_FAILURE;
    }
    fprintf(stdout, "probe: complete\n");
    return EXIT_SUCCESS;
}
