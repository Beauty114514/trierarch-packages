#define _POSIX_C_SOURCE 200809L

#include <gbm.h>
#include <wayland-client.h>

#include "linux-dmabuf-v1-client-protocol.h"

#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

enum {
    BUFFER_WIDTH = 64,
    BUFFER_HEIGHT = 64,
    BYTES_PER_PIXEL = 4,
};

struct globals {
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct zwp_linux_dmabuf_v1 *dmabuf;
    uint32_t dmabuf_version;
};

static void registry_global(void *data, struct wl_registry *registry,
        uint32_t name, const char *interface, uint32_t version) {
    struct globals *globals = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        globals->compositor = wl_registry_bind(registry, name,
                &wl_compositor_interface, version < 4 ? version : 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        globals->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
        globals->dmabuf_version = version < 3 ? version : 3;
        globals->dmabuf = wl_registry_bind(registry, name,
                &zwp_linux_dmabuf_v1_interface, globals->dmabuf_version);
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
    char path[] = "/tmp/trierarch-dmabuf-probe-XXXXXX";
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

static struct wl_buffer *create_shm_buffer(struct wl_shm *shm) {
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
        pixels[index] = 0x0000ff00u;
    munmap(pixels, size);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int)size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0,
            BUFFER_WIDTH, BUFFER_HEIGHT, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

static int modifier_supported(uint64_t modifier) {
    return modifier == DRM_FORMAT_MOD_LINEAR || modifier == DRM_FORMAT_MOD_INVALID;
}

static struct wl_buffer *create_dmabuf_buffer(struct globals *globals,
        struct gbm_bo *buffer) {
    uint64_t modifier = gbm_bo_get_modifier(buffer);
    if (!modifier_supported(modifier)) {
        fprintf(stderr, "GBM modifier 0x%llx is not advertised by Trierarch.\n",
                (unsigned long long)modifier);
        return NULL;
    }
    int fd = gbm_bo_get_fd(buffer);
    if (fd < 0) {
        fprintf(stderr, "gbm_bo_get_fd failed.\n");
        return NULL;
    }
    struct zwp_linux_buffer_params_v1 *params =
            zwp_linux_dmabuf_v1_create_params(globals->dmabuf);
    zwp_linux_buffer_params_v1_add(params, fd, 0, 0, gbm_bo_get_stride(buffer),
            (uint32_t)(modifier >> 32), (uint32_t)modifier);
    struct wl_buffer *wl_buffer = zwp_linux_buffer_params_v1_create_immed(params,
            BUFFER_WIDTH, BUFFER_HEIGHT, DRM_FORMAT_XRGB8888, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    return wl_buffer;
}

static int commit_buffer(struct wl_display *display, struct wl_surface *surface,
        struct wl_buffer *buffer, const char *state) {
    wl_surface_attach(surface, buffer, 0, 0);
    if (buffer)
        wl_surface_damage_buffer(surface, 0, 0, BUFFER_WIDTH, BUFFER_HEIGHT);
    wl_surface_commit(surface);
    if (wl_display_roundtrip(display) < 0)
        return -1;
    fprintf(stdout, "%s (host acknowledged)\n", state);
    return 0;
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
    if (wl_display_roundtrip(display) < 0 || !globals.compositor || !globals.shm ||
            !globals.dmabuf || globals.dmabuf_version < 2) {
        fprintf(stderr, "Host does not provide wl_compositor, wl_shm, and linux-dmabuf v2+.\n");
        return EXIT_FAILURE;
    }
    int render_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    struct gbm_device *device = render_fd >= 0 ? gbm_create_device(render_fd) : NULL;
    struct gbm_bo *gbm_buffer = device ? gbm_bo_create(device, BUFFER_WIDTH, BUFFER_HEIGHT,
            GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR) : NULL;
    struct wl_surface *surface = wl_compositor_create_surface(globals.compositor);
    struct wl_buffer *dmabuf_buffer = gbm_buffer ? create_dmabuf_buffer(&globals, gbm_buffer) : NULL;
    struct wl_buffer *shm_buffer = create_shm_buffer(globals.shm);
    if (!device || !gbm_buffer || !surface || !dmabuf_buffer || !shm_buffer) {
        fprintf(stderr, "Unable to allocate GBM, DMA-BUF, surface, or SHM test buffer.\n");
        return EXIT_FAILURE;
    }
    int status = 0;
    status |= commit_buffer(display, surface, dmabuf_buffer, "probe: attached GBM DMA-BUF");
    sleep_ms(hold_ms);
    status |= commit_buffer(display, surface, NULL, "probe: committed attach(NULL)");
    sleep_ms(hold_ms);
    status |= commit_buffer(display, surface, shm_buffer, "probe: attached green SHM buffer");
    sleep_ms(hold_ms);
    wl_buffer_destroy(shm_buffer);
    wl_buffer_destroy(dmabuf_buffer);
    wl_surface_destroy(surface);
    gbm_bo_destroy(gbm_buffer);
    gbm_device_destroy(device);
    close(render_fd);
    zwp_linux_dmabuf_v1_destroy(globals.dmabuf);
    wl_shm_destroy(globals.shm);
    wl_compositor_destroy(globals.compositor);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    if (status != 0) {
        fprintf(stderr, "Wayland commit roundtrip failed.\n");
        return EXIT_FAILURE;
    }
    fprintf(stdout, "probe: complete\n");
    return EXIT_SUCCESS;
}
