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
#include "wlr-layer-shell-unstable-v1-server-protocol.h"
#include "linux-explicit-synchronization-unstable-v1-server-protocol.h"

struct renderer_context;

/* Keep these in sync with the protocol behavior implemented by this host.
 * wl_resource versions are negotiated per client, so raising the advertised
 * maximum does not force older clients to use newer protocol requests. */
#define TRIERARCH_WL_COMPOSITOR_VERSION 6
#define TRIERARCH_WL_SEAT_VERSION 9

/* Keep the packed 32-bit formats shared by the dma-buf and renderer paths.
 * On little-endian systems the byte order exposed by GL_RGBA differs between
 * the ARGB/XRGB and ABGR/XBGR families, so the renderer must inspect the
 * advertised format before applying its R/B swizzle. */
#define TRIERARCH_DRM_FORMAT_XRGB8888 0x34325258u
#define TRIERARCH_DRM_FORMAT_ARGB8888 0x34325241u
#define TRIERARCH_DRM_FORMAT_XBGR8888 0x34324258u
#define TRIERARCH_DRM_FORMAT_ABGR8888 0x34324241u

struct shm_buffer {
    unsigned int references;
    void (*destroy)(struct shm_buffer *buffer);
    struct wl_resource *resource;
    struct shm_pool *pool;
    void *data;
    size_t size;
    int32_t offset;
    int32_t width;
    int32_t height;
    int32_t stride;
    uint32_t format;
    /* Number of surfaces whose committed state still uses this buffer.  A
     * wl_buffer may be attached to more than one surface, so release must be
     * sent only when the last user drops it. */
    unsigned int busy_users;
    bool single_pixel;
    bool dmabuf;
    /* Guest fd is a memfd token from the virgl buffer shim; the host owns the
     * real AHardwareBuffer and no CPU mapping/EGL import is possible. */
    bool tvg_token;
    bool egl_buffer;
    bool android_buffer;
    void *egl_resource;
    void *android_hardware_buffer;
    int dmabuf_fd;
    /* Guest token fd for a tvg-token buffer; kept until the buffer is gone. */
    int tvg_fd;
    int acquire_fence_fd;
    struct wl_resource *explicit_release;
    /* EGL buffers are cached per wl_resource so repeated attaches reuse one
     * wrapper instead of allocating a new one every frame. */
    struct wl_list egl_cache_link;
    bool egl_cache_linked;
    void *dmabuf_mapping;
    size_t dmabuf_mapping_size;
    uint32_t dmabuf_offset;
    uint64_t dmabuf_modifier;
    struct wayland_server *server;
    struct wl_listener egl_resource_destroy_listener;
    bool egl_resource_destroy_listener_attached;
    struct wl_list pool_link;
};

struct trierarch_dmabuf_feedback_device;

struct compositor_surface {
    struct wl_list link;
    struct wayland_server *server;
    struct wl_resource *wl_surface;
    struct wl_resource *xdg_surface;
    struct wl_resource *xdg_toplevel;
    struct wl_resource *subsurface;
    struct wl_resource *viewport;
    struct wl_resource *fractional_scale;
    struct wl_resource *layer_surface;
    struct compositor_surface *parent;
    /* A subsurface is linked into its parent's children list.  The list order
     * is the client-defined stacking order from wl_subsurface.place_*(). */
    struct wl_list children;
    struct wl_list subsurface_link;
    int32_t subsurface_x;
    int32_t subsurface_y;
    int32_t buffer_scale;
    struct shm_buffer *current;
    struct shm_buffer *pending;
    bool pending_detach;
    struct wl_resource *explicit_sync;
    int acquire_fence_fd;
    struct wl_resource *pending_explicit_release;
    int32_t width;
    int32_t height;
    int32_t layer_x;
    int32_t layer_y;
    uint32_t layer_kind;
    uint32_t layer_anchor;
    uint32_t layer_width;
    uint32_t layer_height;
    int32_t layer_exclusive_zone;
    int32_t layer_margin_top;
    int32_t layer_margin_right;
    int32_t layer_margin_bottom;
    int32_t layer_margin_left;
    bool layer_configured;
    bool configured;
    bool mapped;
    bool is_cursor;
    bool damaged;
    bool viewport_source_set;
    wl_fixed_t viewport_source_x;
    wl_fixed_t viewport_source_y;
    wl_fixed_t viewport_source_width;
    wl_fixed_t viewport_source_height;
    bool viewport_destination_set;
    int32_t viewport_destination_width;
    int32_t viewport_destination_height;
    /* wl_surface.frame requests belong to the next surface commit, not to an
     * arbitrary host repaint that happens before that commit. */
    struct wl_list pending_frame_callbacks;
    /* Callbacks associated with committed state and awaiting presentation. */
    struct wl_list committed_frame_callbacks;
    /* The committed callbacks captured by the output composition in flight. */
    struct wl_list presented_frame_callbacks;
    /* One-second attribution counters for compositor performance diagnosis. */
    uint64_t perf_commits;
    uint64_t perf_damage;
    uint64_t perf_buffer_replacements;
    uint64_t perf_frame_callbacks_requested;
    uint64_t perf_frame_callbacks_committed;
    uint64_t perf_frame_callbacks_captured;
    uint64_t perf_frame_callbacks_completed;
};

struct surface_frame_callback {
    struct wl_list link;
    struct wl_resource *resource;
};

struct pointer_resource {
    struct wl_list link;
    struct wl_listener destroy_listener;
    struct wl_resource *resource;
};

struct keyboard_resource {
    struct wl_list link;
    struct wl_listener destroy_listener;
    struct wl_resource *resource;
};

struct wayland_server {
    struct wl_display *display;
    struct wl_event_loop *event_loop;
    struct wl_event_source *wake_source;
    struct wl_event_source *telemetry_source;
    struct wl_event_source *repaint_source;
    int wake_fd;
    int telemetry_fd;
    int repaint_fd;
    struct wl_list surfaces;
    /* One compositor-side wrapper per live EGL wl_buffer resource. */
    struct wl_list egl_buffers;
    struct wl_list output_resources;
    struct wl_list xdg_output_resources;
    struct wl_list pointer_resources;
    struct wl_list keyboard_resources;
    struct compositor_surface *pointer_focus;
    struct compositor_surface *keyboard_focus;
    struct compositor_surface *cursor_surface;
    wl_fixed_t pointer_x;
    wl_fixed_t pointer_y;
    int32_t cursor_hotspot_x;
    int32_t cursor_hotspot_y;
    uint32_t pointer_buttons;
    uint32_t keyboard_mods_depressed;
    uint32_t keyboard_mods_locked;
    uint32_t keyboard_pressed;
    bool keyboard_caps_lock;
    bool keyboard_num_lock;
    bool keyboard_scroll_lock;
    bool keyboard_caps_lock_down;
    bool keyboard_num_lock_down;
    bool keyboard_scroll_lock_down;
    bool cursor_visible;
    char *runtime_dir;
    int32_t output_width;
    int32_t output_height;
    uint32_t next_serial;
    bool valid;
    struct renderer_context *renderer;
    bool egl_buffer_supported;
    /* Buffer path requested by the profile.  linux-dmabuf is only ever
     * published when this is true and the guest reported a usable device. */
    bool dmabuf_enabled;
    /* Guest-reported render-node identity for future v4 dma-buf feedback. */
    struct trierarch_dmabuf_feedback_device *dmabuf_feedback_device;
    /* Published only after a valid guest device report is registered. */
    struct wl_global *dmabuf_global;
    /* Output repaint state. Requests are coalesced until the next output tick,
     * so a burst of commits produces at most one composition. */
    bool repaint_needed;
    bool repaint_scheduled;
    bool repaint_ready;
    bool repaint_rendering;
    /* One-second host-side frame-pipeline counters. */
    uint64_t perf_last_report_ns;
    uint64_t perf_dispatch_count;
    uint64_t perf_dispatch_wait_ns;
    uint64_t perf_dispatch_wait_max_ns;
    uint64_t perf_surface_commits;
    uint64_t perf_surface_damage;
    uint64_t perf_surface_commit_generation;
    uint64_t perf_surface_damage_generation;
    uint64_t perf_frame_callbacks_requested;
    uint64_t perf_frame_callbacks_committed;
    uint64_t perf_frame_callbacks_captured;
    uint64_t perf_frame_callbacks_completed;
    uint64_t perf_render_count;
    uint64_t perf_render_without_surface_update;
    uint64_t perf_render_ns;
    uint64_t perf_render_max_ns;
    uint64_t perf_swap_ns;
    uint64_t perf_swap_max_ns;
    uint64_t perf_repaint_requests;
    uint64_t perf_repaint_coalesced;
    uint64_t perf_repaint_started;
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
void trierarch_layer_shell_bind(struct wl_client *, void *, uint32_t, uint32_t);
void trierarch_explicit_sync_bind(struct wl_client *, void *, uint32_t, uint32_t);

struct compositor_surface *trierarch_surface_from_resource(struct wl_resource *resource);
void trierarch_surface_commit(struct compositor_surface *surface);
void trierarch_explicit_sync_wait(struct shm_buffer *buffer);
void trierarch_explicit_sync_buffer_release(struct shm_buffer *buffer);
void trierarch_explicit_sync_adopt_release(struct wl_resource *release,
        struct shm_buffer *buffer);
void trierarch_renderer_wait_idle(struct renderer_context *renderer);
void trierarch_layer_surface_commit(struct compositor_surface *surface);
void trierarch_surface_send_configure(struct compositor_surface *surface);
void trierarch_surface_latch_frame_callbacks(struct wayland_server *server);
void trierarch_surface_requeue_frame_callbacks(struct wayland_server *server);
void trierarch_surface_send_frame_callbacks(struct wayland_server *server, uint32_t time_ms);
void trierarch_wayland_frame_presented(struct wayland_server *server, uint32_t time_ms);

struct shm_buffer *trierarch_shm_buffer_from_resource(struct wl_resource *resource);
void trierarch_buffer_ref(struct shm_buffer *buffer);
void trierarch_buffer_unref(struct shm_buffer *buffer);
void trierarch_buffer_destroy_storage(struct shm_buffer *buffer);
void trierarch_shm_buffer_release(struct shm_buffer *buffer);
struct shm_buffer *trierarch_dmabuf_buffer_from_resource(struct wl_resource *resource);
void trierarch_dmabuf_buffer_release(struct shm_buffer *buffer);
void trierarch_dmabuf_bind(struct wl_client *, void *, uint32_t, uint32_t);
bool trierarch_dmabuf_global_create(struct wayland_server *server);
struct shm_buffer *trierarch_egl_buffer_from_resource(struct wl_resource *resource,
        struct wayland_server *server);
struct shm_buffer *trierarch_android_buffer_from_resource(struct wl_resource *resource);

#endif
