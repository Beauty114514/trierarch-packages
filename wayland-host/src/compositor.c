#include "server_internal.h"
#include "xdg-shell-server-protocol.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <android/log.h>

#define TRIERARCH_TAG "TrierarchWayland"

wayland_server_t *trierarch_wayland_create(const char *runtime_dir) {
    struct wayland_server *server = calloc(1, sizeof(*server));
    if (!server) return NULL;
    server->display = wl_display_create();
    server->event_loop = server->display ? wl_display_get_event_loop(server->display) : NULL;
    if (runtime_dir) {
        size_t length = strlen(runtime_dir) + 1;
        server->runtime_dir = malloc(length);
        if (server->runtime_dir) memcpy(server->runtime_dir, runtime_dir, length);
    }
    if (server->runtime_dir) {
        mkdir(server->runtime_dir, 0777);
        // The directory is bind-mounted into a guest whose numeric UID may
        // differ from the Android app UID owning this socket.
        chmod(server->runtime_dir, 0777);
        setenv("XDG_RUNTIME_DIR", server->runtime_dir, 1);
    }
    server->output_width = 1080;
    server->output_height = 1920;
    server->pointer_x = wl_fixed_from_int(server->output_width / 2);
    server->pointer_y = wl_fixed_from_int(server->output_height / 2);
    server->cursor_visible = true;
    server->next_serial = 1;
    server->egl_buffer_supported = false;
    wl_list_init(&server->surfaces);
    wl_list_init(&server->output_resources);
    wl_list_init(&server->xdg_output_resources);
    wl_list_init(&server->pointer_resources);
    if (!server->display || !server->runtime_dir ||
            wl_display_add_socket(server->display, "wayland-trierarch") < 0) {
        trierarch_wayland_destroy(server);
        return NULL;
    }
    char socket_path[PATH_MAX];
    int socket_length = snprintf(socket_path, sizeof(socket_path),
            "%s/wayland-trierarch", server->runtime_dir);
    if (socket_length > 0 && (size_t)socket_length < sizeof(socket_path)) {
        chmod(socket_path, 0666);
    }
    wl_global_create(server->display, &wl_compositor_interface, 4, server,
            trierarch_surface_bind);
    wl_global_create(server->display, &wl_subcompositor_interface, 1, server,
            trierarch_subcompositor_bind);
    wl_global_create(server->display, &wl_shm_interface, 1, server,
            trierarch_shm_bind);
    /* Do not advertise linux-dmabuf until the Android renderer can import a
     * dma-buf with EGL_EXT_image_dma_buf_import.  Advertising it now makes
     * Qt/Mesa select an EGL dma-buf path although this host logs that import
     * as unavailable; a wl_shm fallback is then more reliable.  Keep the
     * implementation in linux_dmabuf.c for the later GPU-sharing milestone. */
    wl_global_create(server->display, &wp_single_pixel_buffer_manager_v1_interface, 1,
            server, trierarch_single_pixel_bind);
    wl_global_create(server->display, &wp_viewporter_interface, 1,
            server, trierarch_viewporter_bind);
    wl_global_create(server->display, &wl_output_interface, 4, server,
            trierarch_output_bind);
    wl_global_create(server->display, &wl_seat_interface, 7, server,
            trierarch_seat_bind);
    wl_global_create(server->display, &zwp_pointer_constraints_v1_interface, 1,
            server, trierarch_pointer_constraints_bind);
    wl_global_create(server->display, &wp_presentation_interface, 1, server,
            trierarch_presentation_bind);
    wl_global_create(server->display, &xdg_wm_base_interface, 6, server,
            trierarch_xdg_shell_bind);
    wl_global_create(server->display, &zxdg_output_manager_v1_interface, 3,
            server, trierarch_xdg_output_bind);
    wl_global_create(server->display, &zxdg_decoration_manager_v1_interface, 1,
            server, trierarch_xdg_decoration_bind);
    wl_global_create(server->display, &wp_fractional_scale_manager_v1_interface, 1,
            server, trierarch_fractional_scale_bind);
    wl_global_create(server->display, &zwp_relative_pointer_manager_v1_interface, 1,
            server, trierarch_relative_pointer_bind);
    wl_global_create(server->display, &android_wlegl_interface, 1,
            server, trierarch_android_wlegl_bind);
    wl_global_create(server->display, &wl_data_device_manager_interface, 3,
            server, trierarch_data_device_bind);
    server->valid = true;
    return server;
}

void trierarch_wayland_destroy(wayland_server_t *server) {
    if (!server) return;
    if (server->display) wl_display_destroy(server->display);
    free(server->runtime_dir);
    free(server);
}

void trierarch_wayland_dispatch(wayland_server_t *server) {
    if (!server || !server->valid) return;
    wl_display_flush_clients(server->display);
    struct timespec before, after;
    clock_gettime(CLOCK_MONOTONIC, &before);
    wl_event_loop_dispatch(server->event_loop, 10);
    clock_gettime(CLOCK_MONOTONIC, &after);
    uint64_t elapsed_ns = (uint64_t)(after.tv_sec - before.tv_sec) * 1000000000ULL
            + (uint64_t)(after.tv_nsec - before.tv_nsec);
    server->perf_dispatch_count++;
    server->perf_dispatch_wait_ns += elapsed_ns;
    if (elapsed_ns > server->perf_dispatch_wait_max_ns)
        server->perf_dispatch_wait_max_ns = elapsed_ns;
    wl_display_flush_clients(server->display);
}

bool trierarch_wayland_has_surface(wayland_server_t *server) {
    struct compositor_surface *surface;
    if (!server) return false;
    wl_list_for_each(surface, &server->surfaces, link) {
        if (surface->mapped && surface->current) return true;
    }
    return false;
}
