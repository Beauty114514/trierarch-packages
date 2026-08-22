#ifndef TRIERARCH_WAYLAND_SERVER_INTERNAL_H
#define TRIERARCH_WAYLAND_SERVER_INTERNAL_H

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <stdbool.h>
#include <stdint.h>

#include "compositor.h"
#include "single-pixel-buffer-v1-server-protocol.h"
#include "viewporter-server-protocol.h"
#include "pointer-constraints-server-protocol.h"
#include "presentation-time-server-protocol.h"
#include "linux-dmabuf-v1-server-protocol.h"
#include "xdg-output-unstable-v1-server-protocol.h"
#include "xdg-decoration-unstable-v1-server-protocol.h"
#include "fractional-scale-v1-server-protocol.h"
#include "relative-pointer-unstable-v1-server-protocol.h"
#include "android-wlegl-server-protocol.h"

struct shm_buffer {
    struct wl_resource *resource;
    struct shm_pool *pool;
    void *data;
    size_t size;
    int32_t offset;
    int32_t width;
    int32_t height;
    int32_t stride;
    uint32_t format;
    bool busy;
    bool single_pixel;
    bool dmabuf;
    bool egl_buffer;
    bool android_buffer;
    void *egl_resource;
    void *android_hardware_buffer;
    int dmabuf_fd;
    void *dmabuf_mapping;
    size_t dmabuf_mapping_size;
    uint64_t dmabuf_modifier;
    struct wl_list pool_link;
};

struct compositor_surface {
    struct wl_list link;
    struct wayland_server *server;
    struct wl_resource *wl_surface;
    struct wl_resource *xdg_surface;
    struct wl_resource *xdg_toplevel;
    struct wl_resource *subsurface;
    struct wl_resource *viewport;
    struct wl_resource *fractional_scale;
    struct compositor_surface *parent;
    int32_t subsurface_x;
    int32_t subsurface_y;
    int32_t buffer_scale;
    struct shm_buffer *current;
    struct shm_buffer *pending;
    int32_t width;
    int32_t height;
    bool configured;
    bool mapped;
    bool damaged;
    bool viewport_source_set;
    wl_fixed_t viewport_source_x;
    wl_fixed_t viewport_source_y;
    wl_fixed_t viewport_source_width;
    wl_fixed_t viewport_source_height;
    bool viewport_destination_set;
    int32_t viewport_destination_width;
    int32_t viewport_destination_height;
    struct wl_list frame_callbacks;
};

struct surface_frame_callback {
    struct wl_list link;
    struct wl_resource *resource;
};

struct wayland_server {
    struct wl_display *display;
    struct wl_event_loop *event_loop;
    struct wl_list surfaces;
    struct wl_list output_resources;
    struct wl_list xdg_output_resources;
    char *runtime_dir;
    int32_t output_width;
    int32_t output_height;
    uint32_t next_serial;
    bool valid;
    bool egl_buffer_supported;
};

void trierarch_surface_bind(struct wl_client *, void *, uint32_t, uint32_t);
void trierarch_subcompositor_bind(struct wl_client *, void *, uint32_t, uint32_t);
void trierarch_shm_bind(struct wl_client *, void *, uint32_t, uint32_t);
void trierarch_single_pixel_bind(struct wl_client *, void *, uint32_t, uint32_t);
void trierarch_viewporter_bind(struct wl_client *, void *, uint32_t, uint32_t);
void trierarch_output_bind(struct wl_client *, void *, uint32_t, uint32_t);
void trierarch_output_enter_surface(struct wayland_server *, struct wl_resource *);
void trierarch_xdg_output_notify(struct wayland_server *);
void trierarch_xdg_shell_bind(struct wl_client *, void *, uint32_t, uint32_t);
void trierarch_seat_bind(struct wl_client *, void *, uint32_t, uint32_t);
void trierarch_pointer_constraints_bind(struct wl_client *, void *, uint32_t, uint32_t);
void trierarch_presentation_bind(struct wl_client *, void *, uint32_t, uint32_t);
void trierarch_xdg_output_bind(struct wl_client *, void *, uint32_t, uint32_t);
void trierarch_xdg_decoration_bind(struct wl_client *, void *, uint32_t, uint32_t);
void trierarch_fractional_scale_bind(struct wl_client *, void *, uint32_t, uint32_t);
void trierarch_relative_pointer_bind(struct wl_client *, void *, uint32_t, uint32_t);
void trierarch_android_wlegl_bind(struct wl_client *, void *, uint32_t, uint32_t);
void trierarch_data_device_bind(struct wl_client *, void *, uint32_t, uint32_t);

struct compositor_surface *trierarch_surface_from_resource(struct wl_resource *resource);
void trierarch_surface_commit(struct compositor_surface *surface);
void trierarch_surface_send_configure(struct compositor_surface *surface);
void trierarch_surface_send_frame_callbacks(struct wayland_server *server, uint32_t time_ms);

struct shm_buffer *trierarch_shm_buffer_from_resource(struct wl_resource *resource);
void trierarch_shm_buffer_release(struct shm_buffer *buffer);
struct shm_buffer *trierarch_dmabuf_buffer_from_resource(struct wl_resource *resource);
void trierarch_dmabuf_buffer_release(struct shm_buffer *buffer);
void trierarch_dmabuf_bind(struct wl_client *, void *, uint32_t, uint32_t);
struct shm_buffer *trierarch_egl_buffer_from_resource(struct wl_resource *resource,
        struct wayland_server *server);
struct shm_buffer *trierarch_android_buffer_from_resource(struct wl_resource *resource);

#endif
