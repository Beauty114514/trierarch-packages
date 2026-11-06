#include "renderer.h"
#include "server_internal.h"
#include "tvg_scanout.h"
#include "vulkan_presenter.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <android/log.h>
#include <errno.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define TAG "TrierarchRenderer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#ifndef EGL_WAYLAND_BUFFER_WL
#define EGL_WAYLAND_BUFFER_WL 0x31d5
#endif
#ifndef EGL_NATIVE_BUFFER_ANDROID
#define EGL_NATIVE_BUFFER_ANDROID 0x3140
#endif
#ifndef EGL_LINUX_DMA_BUF_EXT
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#endif
#ifndef EGL_LINUX_DRM_FOURCC_EXT
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#endif
#ifndef EGL_DMA_BUF_PLANE0_FD_EXT
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#endif
#ifndef EGL_DMA_BUF_PLANE0_OFFSET_EXT
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#endif
#ifndef EGL_DMA_BUF_PLANE0_PITCH_EXT
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
#endif
#ifndef EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT
#define EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT 0x3443
#endif
#ifndef EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT
#define EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT 0x3444
#endif
#ifndef EGL_SYNC_FENCE_KHR
#define EGL_SYNC_FENCE_KHR 0x30F9
#endif
#ifndef EGL_SYNC_FLUSH_COMMANDS_BIT_KHR
#define EGL_SYNC_FLUSH_COMMANDS_BIT_KHR 0x0001
#endif
#ifndef EGL_FOREVER_KHR
#define EGL_FOREVER_KHR 0xFFFFFFFFFFFFFFFFULL
#endif
#ifndef EGL_NO_SYNC_KHR
#define EGL_NO_SYNC_KHR ((EGLSyncKHR)0)
#endif
#ifndef EGL_ALREADY_SIGNALED_KHR
#define EGL_ALREADY_SIGNALED_KHR 0x30F2
#endif

#define DRM_FORMAT_MOD_INVALID 0x00ffffffffffffffULL

/* Modifier 0 is DRM_FORMAT_MOD_LINEAR.  An invalid modifier means that the
 * producer did not provide a layout that EGL can safely interpret. */
static bool dmabuf_modifier_is_linear(uint64_t modifier) {
    return modifier == 0;
}

static bool format_needs_rb_swizzle(uint32_t format) {
    return format == TRIERARCH_DRM_FORMAT_XRGB8888 ||
            format == TRIERARCH_DRM_FORMAT_ARGB8888;
}

static bool format_is_opaque(uint32_t format) {
    return format == TRIERARCH_DRM_FORMAT_XRGB8888 ||
            format == TRIERARCH_DRM_FORMAT_XBGR8888;
}

/* mmap alone does not establish CPU/GPU ownership of a dma-buf.  The guest
 * compositor may still be rendering when the Android host falls back to a
 * CPU upload, so bracket that read with the dma-buf synchronization ioctl. */
static void maybe_probe_dmabuf(struct shm_buffer *buffer) {
    static bool attempted;
    if (attempted || !buffer || !buffer->dmabuf) return;
    attempted = true;
    if (!trierarch_vulkan_dmabuf_probe_configured()) return;
    trierarch_vulkan_probe_dmabuf(buffer->dmabuf_fd, buffer->width, buffer->height,
            buffer->stride, buffer->format, buffer->dmabuf_modifier);
}

static bool dmabuf_begin_cpu_read(const struct shm_buffer *buffer) {
    if (!buffer || !buffer->dmabuf || buffer->dmabuf_fd < 0)
        return false;
    struct dma_buf_sync sync = {
        .flags = DMA_BUF_SYNC_READ | DMA_BUF_SYNC_START,
    };
    if (ioctl(buffer->dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync) == 0) {
        if (buffer->server) buffer->server->diag_cpu_sync_ok++;
        return true;
    }
    if (buffer->server) buffer->server->diag_cpu_sync_fail++;
    static bool reported;
    if (!reported) {
        reported = true;
        LOGE("dma-buf CPU read synchronization unavailable: errno=%d", errno);
    }
    return false;
}

static void dmabuf_end_cpu_read(const struct shm_buffer *buffer) {
    if (!buffer || !buffer->dmabuf || buffer->dmabuf_fd < 0)
        return;
    struct dma_buf_sync sync = {
        .flags = DMA_BUF_SYNC_READ | DMA_BUF_SYNC_END,
    };
    if (ioctl(buffer->dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync) != 0) {
        if (buffer->server) buffer->server->diag_cpu_sync_fail++;
        static bool reported;
        if (!reported) {
            reported = true;
            LOGE("dma-buf CPU read synchronization end failed: errno=%d", errno);
        }
    } else if (buffer->server) {
        buffer->server->diag_cpu_sync_ok++;
    }
}

typedef EGLBoolean (*bind_wayland_display_fn)(EGLDisplay, void *);
typedef EGLBoolean (*query_wayland_buffer_fn)(EGLDisplay, void *, EGLint, EGLint *);
typedef EGLImageKHR (*create_image_fn)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
typedef EGLBoolean (*destroy_image_fn)(EGLDisplay, EGLImageKHR);
typedef void (*image_target_fn)(GLenum, GLeglImageOES);
typedef EGLClientBuffer (*native_client_buffer_fn)(void *);
typedef EGLSyncKHR (*create_sync_fn)(EGLDisplay, EGLenum, const EGLint *);
typedef EGLBoolean (*destroy_sync_fn)(EGLDisplay, EGLSyncKHR);
typedef EGLint (*client_wait_sync_fn)(EGLDisplay, EGLSyncKHR, EGLint, EGLTimeKHR);
typedef EGLint (*dup_native_fence_fd_fn)(EGLDisplay, EGLSyncKHR);

struct renderer_context {
    struct wayland_server *server;
    EGLDisplay display;
    EGLSurface surface;
    EGLContext context;
    ANativeWindow *window;
    void *vulkan;
    GLuint background_program;
    GLuint texture_program;
    GLuint texture;
    int width;
    int height;
    bool valid;
    create_image_fn create_image;
    destroy_image_fn destroy_image;
    image_target_fn image_target;
    native_client_buffer_fn native_client_buffer;
    create_sync_fn create_sync;
    destroy_sync_fn destroy_sync;
    client_wait_sync_fn client_wait_sync;
    dup_native_fence_fd_fn dup_native_fence_fd;
    EGLSyncKHR frame_fence;
    bool frame_fence_created;
    bool dmabuf_import_supported;
    uint64_t observed_surface_commits;
    uint64_t observed_surface_damage;
    uint64_t frame_sequence;
    const void *last_buffer;
    int last_width;
    int last_height;
    uint32_t last_format;
    uint64_t last_modifier;
    const char *last_import_path;
};

static uint64_t monotonic_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static void record_max(uint64_t value, uint64_t *maximum) {
    if (value > *maximum) *maximum = value;
}

static void report_surface_activity(struct wayland_server *server) {
    struct compositor_surface *surface;
    wl_list_for_each(surface, &server->surfaces, link) {
        if (!surface->perf_commits && !surface->perf_damage &&
                !surface->perf_buffer_replacements && !surface->perf_frame_callbacks_requested &&
                !surface->perf_frame_callbacks_committed &&
                !surface->perf_frame_callbacks_captured && !surface->perf_frame_callbacks_completed)
            continue;
        const char *role = surface->is_cursor ? "cursor" :
                surface->parent ? "subsurface" : "root";
        struct wl_client *client = surface->wl_surface
                ? wl_resource_get_client(surface->wl_surface) : NULL;
        LOGI("perf-surface surface=%p client=%p role=%s parent=%p mapped=%d "
                "buffer=%dx%d commits=%llu damage=%llu replacements=%llu "
                "callbacks=req/commit/capture/done=%llu/%llu/%llu/%llu",
                (void *)surface, (void *)client, role, (void *)surface->parent,
                surface->mapped, surface->width, surface->height,
                (unsigned long long)surface->perf_commits,
                (unsigned long long)surface->perf_damage,
                (unsigned long long)surface->perf_buffer_replacements,
                (unsigned long long)surface->perf_frame_callbacks_requested,
                (unsigned long long)surface->perf_frame_callbacks_committed,
                (unsigned long long)surface->perf_frame_callbacks_captured,
                (unsigned long long)surface->perf_frame_callbacks_completed);
        surface->perf_commits = 0;
        surface->perf_damage = 0;
        surface->perf_buffer_replacements = 0;
        surface->perf_frame_callbacks_requested = 0;
        surface->perf_frame_callbacks_committed = 0;
        surface->perf_frame_callbacks_captured = 0;
        surface->perf_frame_callbacks_completed = 0;
    }
}

void trierarch_renderer_report_performance(struct wayland_server *server) {
    if (!server) return;
    uint64_t now_ns = monotonic_ns();
    if (!server->perf_last_report_ns) {
        server->perf_last_report_ns = now_ns;
        return;
    }
    uint64_t interval_ns = now_ns - server->perf_last_report_ns;
    if (interval_ns < 1000000000ULL) return;
    uint64_t dispatch_average_us = server->perf_dispatch_count
            ? server->perf_dispatch_wait_ns / server->perf_dispatch_count / 1000ULL : 0;
    uint64_t render_average_us = server->perf_render_count
            ? server->perf_render_ns / server->perf_render_count / 1000ULL : 0;
    uint64_t swap_average_us = server->perf_render_count
            ? server->perf_swap_ns / server->perf_render_count / 1000ULL : 0;
    LOGI("perf %.2fs dispatch=%llu avg/max=%llu/%lluus repaint=%llu/%llu/%llu "
            "commits=%llu damage=%llu "
            "renders=%llu no-surface-update=%llu render-us avg/max=%llu/%llu "
            "swap-us avg/max=%llu/%llu callbacks=req/commit/capture/done=%llu/%llu/%llu/%llu",
            (double)interval_ns / 1000000000.0,
            (unsigned long long)server->perf_dispatch_count,
            (unsigned long long)dispatch_average_us,
            (unsigned long long)(server->perf_dispatch_wait_max_ns / 1000ULL),
            (unsigned long long)server->perf_repaint_requests,
            (unsigned long long)server->perf_repaint_coalesced,
            (unsigned long long)server->perf_repaint_started,
            (unsigned long long)server->perf_surface_commits,
            (unsigned long long)server->perf_surface_damage,
            (unsigned long long)server->perf_render_count,
            (unsigned long long)server->perf_render_without_surface_update,
            (unsigned long long)render_average_us,
            (unsigned long long)(server->perf_render_max_ns / 1000ULL),
            (unsigned long long)swap_average_us,
            (unsigned long long)(server->perf_swap_max_ns / 1000ULL),
            (unsigned long long)server->perf_frame_callbacks_requested,
            (unsigned long long)server->perf_frame_callbacks_committed,
            (unsigned long long)server->perf_frame_callbacks_captured,
            (unsigned long long)server->perf_frame_callbacks_completed);
    report_surface_activity(server);
    struct renderer_context *diagnostic_renderer = server->renderer;
    LOGI("present swap=%llu/%llu fence=%llu/%llu import=ok:%llu fail:%llu cpu:%llu none:%llu "
            "dmabuf=accept:%llu reject:%llu tvg:%llu deferred=enq:%llu dup:%llu flush:%llu "
            "depth:max:%llu/%llu overflow:%llu "
            "cpu-sync=ok:%llu fail:%llu viewport=%llu acquire=wait:%llu fail:%llu "
            "last-frame=%llu buffer=%p %dx%d fmt=0x%x mod=0x%llx path=%s",
            (unsigned long long)server->diag_swap_ok,
            (unsigned long long)server->diag_swap_fail,
            (unsigned long long)server->diag_fence_created,
            (unsigned long long)server->diag_fence_missing,
            (unsigned long long)server->diag_egl_import_ok,
            (unsigned long long)server->diag_egl_import_fail,
            (unsigned long long)server->diag_cpu_upload,
            (unsigned long long)server->diag_no_image,
            (unsigned long long)server->diag_dmabuf_accept,
            (unsigned long long)server->diag_dmabuf_reject,
            (unsigned long long)server->diag_tvg_buffers,
            (unsigned long long)server->diag_deferred_enqueued,
            (unsigned long long)server->diag_deferred_duplicates,
            (unsigned long long)server->diag_deferred_flushed,
            (unsigned long long)trierarch_deferred_release_count(),
            (unsigned long long)server->diag_deferred_depth_max,
            (unsigned long long)server->diag_deferred_overflow,
            (unsigned long long)server->diag_cpu_sync_ok,
            (unsigned long long)server->diag_cpu_sync_fail,
            (unsigned long long)server->diag_viewport_changes,
            (unsigned long long)server->diag_acquire_fence_waits,
            (unsigned long long)server->diag_acquire_fence_failures,
            (unsigned long long)(diagnostic_renderer ? diagnostic_renderer->frame_sequence : 0),
            diagnostic_renderer ? diagnostic_renderer->last_buffer : NULL,
            diagnostic_renderer ? diagnostic_renderer->last_width : 0,
            diagnostic_renderer ? diagnostic_renderer->last_height : 0,
            diagnostic_renderer ? diagnostic_renderer->last_format : 0,
            diagnostic_renderer ? (unsigned long long)diagnostic_renderer->last_modifier : 0ULL,
            (diagnostic_renderer && diagnostic_renderer->last_import_path)
                    ? diagnostic_renderer->last_import_path : "none");
    server->diag_swap_ok = 0;
    server->diag_swap_fail = 0;
    server->diag_fence_created = 0;
    server->diag_fence_missing = 0;
    server->diag_egl_import_ok = 0;
    server->diag_egl_import_fail = 0;
    server->diag_cpu_upload = 0;
    server->diag_no_image = 0;
    server->diag_dmabuf_accept = 0;
    server->diag_dmabuf_reject = 0;
    server->diag_tvg_buffers = 0;
    server->diag_deferred_enqueued = 0;
    server->diag_deferred_duplicates = 0;
    server->diag_deferred_flushed = 0;
    server->diag_deferred_depth_max = 0;
    server->diag_deferred_overflow = 0;
    server->diag_cpu_sync_ok = 0;
    server->diag_cpu_sync_fail = 0;
    server->diag_viewport_changes = 0;
    server->diag_acquire_fence_waits = 0;
    server->diag_acquire_fence_failures = 0;
    server->perf_last_report_ns = now_ns;
    server->perf_dispatch_count = 0;
    server->perf_dispatch_wait_ns = 0;
    server->perf_dispatch_wait_max_ns = 0;
    server->perf_repaint_requests = 0;
    server->perf_repaint_coalesced = 0;
    server->perf_repaint_started = 0;
    server->perf_surface_commits = 0;
    server->perf_surface_damage = 0;
    server->perf_frame_callbacks_requested = 0;
    server->perf_frame_callbacks_committed = 0;
    server->perf_frame_callbacks_captured = 0;
    server->perf_frame_callbacks_completed = 0;
    server->perf_render_count = 0;
    server->perf_render_without_surface_update = 0;
    server->perf_render_ns = 0;
    server->perf_render_max_ns = 0;
    server->perf_swap_ns = 0;
    server->perf_swap_max_ns = 0;
}

static const char *vertex_source =
        "attribute vec2 position; attribute vec2 texcoord; varying vec2 uv;"
        "void main(){gl_Position=vec4(position,0.0,1.0);uv=texcoord;}";
static const char *background_source =
        "precision mediump float; void main(){gl_FragColor=vec4(0.0,0.0,0.0,1.0);}";
static const char *texture_source =
        "precision mediump float; varying vec2 uv; uniform sampler2D tex;"
        "uniform float swizzle; uniform float opaque;"
        "void main(){vec4 c=texture2D(tex,uv); vec4 b=vec4(c.b,c.g,c.r,c.a);"
        "c=mix(c,b,swizzle); c.a=mix(c.a,1.0,opaque); gl_FragColor=c;}";

static GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        LOGE("shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint make_program(const char *fragment) {
    GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_source);
    GLuint frag = compile_shader(GL_FRAGMENT_SHADER, fragment);
    if (!vertex || !frag) {
        if (vertex) glDeleteShader(vertex);
        if (frag) glDeleteShader(frag);
        return 0;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, frag);
    glBindAttribLocation(program, 0, "position");
    glBindAttribLocation(program, 1, "texcoord");
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(frag);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

struct renderer_context *trierarch_renderer_create(ANativeWindow *window,
        struct wayland_server *server) {
    if (!window) return NULL;
    if (trierarch_vulkan_presenter_configured()) {
        struct renderer_context *vulkan_renderer = calloc(1, sizeof(*vulkan_renderer));
        if (vulkan_renderer) {
            vulkan_renderer->window = window;
            vulkan_renderer->server = server;
            vulkan_renderer->vulkan = trierarch_vulkan_presenter_create(window);
            if (vulkan_renderer->vulkan) {
                vulkan_renderer->valid = true;
                server->renderer = vulkan_renderer;
                LOGI("Vulkan renderer active; EGL renderer not created");
                return vulkan_renderer;
            }
            free(vulkan_renderer);
        }
        LOGI("Vulkan renderer unavailable; falling back to EGL");
    }
    struct renderer_context *renderer = calloc(1, sizeof(*renderer));
    if (!renderer) return NULL;
    renderer->display = EGL_NO_DISPLAY;
    renderer->surface = EGL_NO_SURFACE;
    renderer->context = EGL_NO_CONTEXT;
    renderer->window = window;
    renderer->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (renderer->display == EGL_NO_DISPLAY) goto fail;
    if (!eglInitialize(renderer->display, NULL, NULL)) goto fail;
    const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8, EGL_NONE,
    };
    EGLConfig config;
    EGLint count = 0;
    if (!eglChooseConfig(renderer->display, config_attributes, &config, 1, &count) || count == 0)
        goto fail;
    const EGLint context_attributes[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    renderer->context = eglCreateContext(renderer->display, config, EGL_NO_CONTEXT,
            context_attributes);
    if (renderer->context == EGL_NO_CONTEXT) goto fail;
    renderer->surface = eglCreateWindowSurface(renderer->display, config, window, NULL);
    if (renderer->surface == EGL_NO_SURFACE) goto fail;
    if (!eglMakeCurrent(renderer->display, renderer->surface, renderer->surface, renderer->context))
        goto fail;

    renderer->create_image = (create_image_fn)eglGetProcAddress("eglCreateImageKHR");
    renderer->destroy_image = (destroy_image_fn)eglGetProcAddress("eglDestroyImageKHR");
    renderer->image_target = (image_target_fn)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    renderer->native_client_buffer = (native_client_buffer_fn)eglGetProcAddress(
            "eglGetNativeClientBufferANDROID");
    renderer->create_sync = (create_sync_fn)eglGetProcAddress("eglCreateSyncKHR");
    renderer->destroy_sync = (destroy_sync_fn)eglGetProcAddress("eglDestroySyncKHR");
    renderer->client_wait_sync = (client_wait_sync_fn)eglGetProcAddress("eglClientWaitSyncKHR");
    renderer->dup_native_fence_fd = (dup_native_fence_fd_fn)eglGetProcAddress(
            "eglDupNativeFenceFDANDROID");
    const char *extensions = eglQueryString(renderer->display, EGL_EXTENSIONS);
    const bool has_dmabuf_import = extensions &&
            strstr(extensions, "EGL_EXT_image_dma_buf_import") != NULL;
    const bool has_dmabuf_modifiers = extensions &&
            strstr(extensions, "EGL_EXT_image_dma_buf_import_modifiers") != NULL;
    const bool has_egl_wayland = extensions &&
            strstr(extensions, "EGL_WL_bind_wayland_display") != NULL;
    renderer->dmabuf_import_supported = has_dmabuf_import &&
            renderer->create_image && renderer->destroy_image && renderer->image_target;
    LOGI("EGL extensions: %s", extensions ? extensions : "(null)");
    LOGI("EGL capability dmabuf=%d modifiers=%d egl-wayland=%d create_image=%d",
            has_dmabuf_import ? 1 : 0, has_dmabuf_modifiers ? 1 : 0,
            has_egl_wayland ? 1 : 0, renderer->create_image ? 1 : 0);
    bind_wayland_display_fn bind =
            (bind_wayland_display_fn)eglGetProcAddress("eglBindWaylandDisplayWL");
    query_wayland_buffer_fn query =
            (query_wayland_buffer_fn)eglGetProcAddress("eglQueryWaylandBufferWL");
    if (bind && query && renderer->create_image && renderer->destroy_image &&
            renderer->image_target && server && bind(renderer->display, server->display)) {
        server->egl_buffer_supported = true;
        LOGI("EGL Wayland buffer import enabled");
    } else {
        LOGI("EGL Wayland buffer import unavailable; SHM/dmabuf fallback remains active");
    }
    LOGI("EGL frame synchronization %s",
            renderer->create_sync && renderer->destroy_sync && renderer->client_wait_sync
                    ? "available" : "unavailable");
    LOGI("EGL dma-buf import %s", renderer->dmabuf_import_supported ? "available" : "unavailable");
    eglQuerySurface(renderer->display, renderer->surface, EGL_WIDTH, &renderer->width);
    eglQuerySurface(renderer->display, renderer->surface, EGL_HEIGHT, &renderer->height);
    renderer->background_program = make_program(background_source);
    renderer->texture_program = make_program(texture_source);
    glGenTextures(1, &renderer->texture);
    renderer->valid = renderer->background_program && renderer->texture_program && renderer->texture;
    eglMakeCurrent(renderer->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (!renderer->valid) goto fail;
    renderer->server = server;
    server->renderer = renderer;
    LOGI("renderer created window=%p size=%dx%d server=%p", (void *)window,
            renderer->width, renderer->height, (void *)server);
    return renderer;

fail:
    trierarch_renderer_destroy(renderer);
    return NULL;
}

void trierarch_renderer_destroy(struct renderer_context *renderer) {
    if (!renderer) return;
    LOGI("renderer destroy window=%p size=%dx%d server=%p vulkan=%d", (void *)renderer->window,
            renderer->width, renderer->height, (void *)renderer->server,
            renderer->vulkan ? 1 : 0);
    if (renderer->server && renderer->server->renderer == renderer)
        renderer->server->renderer = NULL;
    if (renderer->vulkan) {
        trierarch_vulkan_presenter_wait_idle(
                (trierarch_vulkan_presenter *)renderer->vulkan);
        trierarch_flush_deferred_releases();
        trierarch_vulkan_presenter_destroy((trierarch_vulkan_presenter *)renderer->vulkan);
        free(renderer);
        return;
    }
    trierarch_renderer_wait_idle(renderer);
    trierarch_flush_deferred_releases();
    if (renderer->frame_fence != EGL_NO_SYNC_KHR && renderer->destroy_sync) {
        renderer->destroy_sync(renderer->display, renderer->frame_fence);
        renderer->frame_fence = EGL_NO_SYNC_KHR;
    }
    if (renderer->display != EGL_NO_DISPLAY && renderer->surface != EGL_NO_SURFACE &&
            renderer->context != EGL_NO_CONTEXT) {
        eglMakeCurrent(renderer->display, renderer->surface, renderer->surface,
                renderer->context);
        if (renderer->texture) glDeleteTextures(1, &renderer->texture);
        if (renderer->background_program) glDeleteProgram(renderer->background_program);
        if (renderer->texture_program) glDeleteProgram(renderer->texture_program);
        eglMakeCurrent(renderer->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (renderer->display != EGL_NO_DISPLAY) {
        if (renderer->surface != EGL_NO_SURFACE) eglDestroySurface(renderer->display, renderer->surface);
        if (renderer->context != EGL_NO_CONTEXT) eglDestroyContext(renderer->display, renderer->context);
        eglTerminate(renderer->display);
    }
    free(renderer);
}

void trierarch_renderer_wait_idle(struct renderer_context *renderer) {
    if (!renderer || renderer->frame_fence == EGL_NO_SYNC_KHR ||
            !renderer->client_wait_sync || !renderer->destroy_sync)
        return;
    /* Never block the host render loop forever on a guest/GPU fence.  A
     * stuck fence must not pin deferred guest buffers and leave KWin waiting
     * in an uninterruptible state.  The timeout is a recovery bound; the
     * fence is still destroyed so the next frame can make progress. */
    const EGLTime timeout_ns = 1000000000ULL;
    EGLint result = renderer->client_wait_sync(renderer->display, renderer->frame_fence,
            EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, timeout_ns);
    if (result != EGL_CONDITION_SATISFIED_KHR && result != EGL_ALREADY_SIGNALED_KHR) {
        LOGE("EGL frame fence wait failed or timed out: result=0x%x", result);
    }
    renderer->destroy_sync(renderer->display, renderer->frame_fence);
    renderer->frame_fence = EGL_NO_SYNC_KHR;
    renderer->frame_fence_created = false;
}

bool trierarch_renderer_valid(const struct renderer_context *renderer) {
    return renderer && (renderer->valid || renderer->vulkan);
}

static void draw_surface(struct renderer_context *renderer,
        struct compositor_surface *surface, int x, int y) {
    struct shm_buffer *buffer = surface->current;
    if (!buffer) return;
    if (buffer->dmabuf) maybe_probe_dmabuf(buffer);
    const char *last_path = "none";
    if (buffer->dmabuf) {
        static unsigned int trace_count;
        if (trace_count++ < 3) {
            LOGI("dmabuf trace stage=draw mapped=%d importer=%d size=%dx%d",
                    surface->mapped, buffer->dmabuf,
                    buffer->width, buffer->height);
        }
    }
    int scale = surface->buffer_scale > 0 ? surface->buffer_scale : 1;
    int width = surface->viewport_destination_set ? surface->viewport_destination_width : buffer->width / scale;
    int height = surface->viewport_destination_set ? surface->viewport_destination_height : buffer->height / scale;
    if (width <= 0 || height <= 0) return;
    float left = 2.0f * x / renderer->width - 1.0f;
    float right = 2.0f * (x + width) / renderer->width - 1.0f;
    float top = 1.0f - 2.0f * y / renderer->height;
    float bottom = 1.0f - 2.0f * (y + height) / renderer->height;
    glUseProgram(renderer->texture_program);
    glBindTexture(GL_TEXTURE_2D, renderer->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    float swizzle = 1.0f, opaque = 1.0f;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    if (buffer->android_buffer && buffer->android_hardware_buffer &&
            renderer->create_image && renderer->image_target && renderer->native_client_buffer) {
        EGLClientBuffer native_buffer = renderer->native_client_buffer(
                buffer->android_hardware_buffer);
        const EGLint attributes[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
        if (native_buffer) image = renderer->create_image(renderer->display, EGL_NO_CONTEXT,
                EGL_NATIVE_BUFFER_ANDROID, native_buffer, attributes);
        if (image != EGL_NO_IMAGE_KHR) {
            renderer->image_target(GL_TEXTURE_2D, image);
            if (glGetError() == GL_NO_ERROR) {
                if (renderer->server) renderer->server->diag_egl_import_ok++;
                if (renderer->server && renderer->server->diag_egl_import_ok <= 8)
                    LOGI("dma-buf import path=ahb %dx%d fmt=0x%x modifier=0x%llx",
                            buffer->width, buffer->height, buffer->format,
                            (unsigned long long)buffer->dmabuf_modifier);
                swizzle = 0.0f;
                opaque = 0.0f;
                last_path = "ahb";
            } else {
                if (renderer->server) renderer->server->diag_egl_import_fail++;
                renderer->destroy_image(renderer->display, image);
                image = EGL_NO_IMAGE_KHR;
            }
        }
    }
    if (image == EGL_NO_IMAGE_KHR && buffer->egl_buffer && buffer->egl_resource &&
            renderer->create_image && renderer->image_target) {
        image = renderer->create_image(renderer->display, renderer->context,
                EGL_WAYLAND_BUFFER_WL, (EGLClientBuffer)buffer->egl_resource, NULL);
        if (image != EGL_NO_IMAGE_KHR) {
            renderer->image_target(GL_TEXTURE_2D, image);
            if (renderer->server) renderer->server->diag_egl_import_ok++;
            if (renderer->server && renderer->server->diag_egl_import_ok <= 8)
                LOGI("dma-buf import path=egl-wayland %dx%d fmt=0x%x",
                        buffer->width, buffer->height, buffer->format);
            swizzle = 0.0f;
            opaque = 0.0f;
            last_path = "egl-wayland";
        }
    }
    if (image == EGL_NO_IMAGE_KHR && buffer->dmabuf && buffer->dmabuf_fd >= 0 &&
            renderer->dmabuf_import_supported) {
        /* Never let EGL silently reinterpret a tiled/compressed buffer as
         * linear. That can return a valid EGLImage while producing the
         * characteristic repeated-column corruption on screen. */
        if (dmabuf_modifier_is_linear(buffer->dmabuf_modifier)) {
            int fd = dup(buffer->dmabuf_fd);
            if (fd >= 0) {
                const EGLint basic_attributes[] = {
                    EGL_WIDTH, buffer->width,
                    EGL_HEIGHT, buffer->height,
                    EGL_LINUX_DRM_FOURCC_EXT, (EGLint)buffer->format,
                    EGL_DMA_BUF_PLANE0_FD_EXT, fd,
                    EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)buffer->dmabuf_offset,
                    EGL_DMA_BUF_PLANE0_PITCH_EXT, buffer->stride,
                    EGL_NONE,
                };
                image = renderer->create_image(renderer->display, EGL_NO_CONTEXT,
                        EGL_LINUX_DMA_BUF_EXT, NULL, basic_attributes);
                close(fd);
            }
        } else if (buffer->dmabuf_modifier != DRM_FORMAT_MOD_INVALID) {
            int fd = dup(buffer->dmabuf_fd);
            if (fd >= 0) {
                const EGLint modifier_attributes[] = {
                    EGL_WIDTH, buffer->width,
                    EGL_HEIGHT, buffer->height,
                    EGL_LINUX_DRM_FOURCC_EXT, (EGLint)buffer->format,
                    EGL_DMA_BUF_PLANE0_FD_EXT, fd,
                    EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)buffer->dmabuf_offset,
                    EGL_DMA_BUF_PLANE0_PITCH_EXT, buffer->stride,
                    EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(uint32_t)buffer->dmabuf_modifier,
                    EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(uint32_t)(buffer->dmabuf_modifier >> 32),
                    EGL_NONE,
                };
                image = renderer->create_image(renderer->display, EGL_NO_CONTEXT,
                        EGL_LINUX_DMA_BUF_EXT, NULL, modifier_attributes);
                close(fd);
            }
        } else {
            LOGI("dmabuf EGL import skipped: unknown modifier fmt=0x%x stride=%d",
                    buffer->format, buffer->stride);
        }
        if (image != EGL_NO_IMAGE_KHR) {
            glBindTexture(GL_TEXTURE_2D, renderer->texture);
            renderer->image_target(GL_TEXTURE_2D, image);
            if (glGetError() == GL_NO_ERROR) {
                swizzle = 0.0f;
                opaque = format_is_opaque(buffer->format) ? 1.0f : 0.0f;
                last_path = "dma-buf";
                if (renderer->server) renderer->server->diag_egl_import_ok++;
                if (renderer->server && renderer->server->diag_egl_import_ok <= 8)
                    LOGI("dma-buf import path=dma-buf %dx%d fmt=0x%x modifier=0x%llx stride=%d",
                            buffer->width, buffer->height, buffer->format,
                            (unsigned long long)buffer->dmabuf_modifier, buffer->stride);
            } else {
                if (renderer->server) renderer->server->diag_egl_import_fail++;
                renderer->destroy_image(renderer->display, image);
                image = EGL_NO_IMAGE_KHR;
            }
        } else {
            if (renderer->server) renderer->server->diag_egl_import_fail++;
            if (renderer->server && renderer->server->diag_egl_import_fail <= 8)
                LOGE("dma-buf import failed path=dma-buf %dx%d fmt=0x%x modifier=0x%llx stride=%d; CPU fallback",
                        buffer->width, buffer->height, buffer->format,
                        (unsigned long long)buffer->dmabuf_modifier, buffer->stride);
        }
    }
    /* mmap() exposes the real pixels only for a linear dma-buf.  A tiled or
     * otherwise opaque layout must not fall through to glTexImage2D either;
     * that would reproduce the same corruption with a different path. */
    const bool cpu_mapping_safe = !buffer->dmabuf ||
            dmabuf_modifier_is_linear(buffer->dmabuf_modifier);
    if (image == EGL_NO_IMAGE_KHR && buffer->data && cpu_mapping_safe) {
        const bool cpu_read_active = dmabuf_begin_cpu_read(buffer);
        const int packed_stride = buffer->width * 4;
        const void *pixels = buffer->data;
        void *packed = NULL;
        if (buffer->stride != packed_stride) {
            packed = malloc((size_t)packed_stride * (size_t)buffer->height);
            if (!packed) {
                if (cpu_read_active)
                    dmabuf_end_cpu_read(buffer);
                return;
            }
            for (int row = 0; row < buffer->height; ++row) {
                memcpy((char *)packed + (size_t)row * (size_t)packed_stride,
                        (const char *)buffer->data + (size_t)row * (size_t)buffer->stride,
                        (size_t)packed_stride);
            }
            pixels = packed;
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, buffer->width, buffer->height, 0,
                GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        free(packed);
        last_path = "cpu";
        if (renderer->server) renderer->server->diag_cpu_upload++;
        if (renderer->server && renderer->server->diag_cpu_upload <= 8)
            LOGI("dma-buf import path=cpu %dx%d fmt=0x%x stride=%d modifier=0x%llx",
                    buffer->width, buffer->height, buffer->format, buffer->stride,
                    (unsigned long long)buffer->dmabuf_modifier);
        if (cpu_read_active)
            dmabuf_end_cpu_read(buffer);
        /* XRGB has no alpha channel. ARGB must retain its alpha: cursor images
         * rely on transparent pixels around the visible pointer shape. */
        swizzle = format_needs_rb_swizzle(buffer->format) ? 1.0f : 0.0f;
        opaque = format_is_opaque(buffer->format) ? 1.0f : 0.0f;
    }
    if (image == EGL_NO_IMAGE_KHR && !buffer->data) {
        static unsigned int unavailable_buffer_count;
        if (renderer->server) renderer->server->diag_no_image++;
        if (unavailable_buffer_count++ < 3) {
            LOGE("skipping dma-buf without an EGL import or CPU mapping: %dx%d fmt=0x%x",
                    buffer->width, buffer->height, buffer->format);
        }
        return;
    }
    glUniform1f(glGetUniformLocation(renderer->texture_program, "swizzle"), swizzle);
    glUniform1f(glGetUniformLocation(renderer->texture_program, "opaque"), opaque);
    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
    if (surface->viewport_source_set && buffer->width > 0 && buffer->height > 0) {
        u0 = wl_fixed_to_double(surface->viewport_source_x) / buffer->width;
        v0 = wl_fixed_to_double(surface->viewport_source_y) / buffer->height;
        u1 = u0 + wl_fixed_to_double(surface->viewport_source_width) / buffer->width;
        v1 = v0 + wl_fixed_to_double(surface->viewport_source_height) / buffer->height;
    }
    const GLfloat vertices[] = { left,bottom,u0,v1, right,bottom,u1,v1,
            left,top,u0,v0, right,top,u1,v0 };
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,4*sizeof(float),vertices);
    glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,4*sizeof(float),vertices+2);
    glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    glDisableVertexAttribArray(0); glDisableVertexAttribArray(1);
    if (!surface->parent && surface != renderer->server->cursor_surface) {
        renderer->last_buffer = buffer;
        renderer->last_width = buffer->width;
        renderer->last_height = buffer->height;
        renderer->last_format = buffer->format;
        renderer->last_modifier = buffer->dmabuf_modifier;
        renderer->last_import_path = last_path;
    }
    if (image != EGL_NO_IMAGE_KHR && renderer->destroy_image) renderer->destroy_image(renderer->display,image);
    /* `surface->current` remains the source for later redraws until a new
     * wl_surface.commit replaces it.  Releasing here would let the client
     * destroy or reuse the wl_buffer while this surface still points at it.
     * surface_commit()/surface_resource_destroy() release it at the point
     * where the compositor actually stops using it. */
}

static void draw_surface_tree(struct renderer_context *renderer,
        struct wayland_server *server, struct compositor_surface *surface,
        int parent_x, int parent_y) {
    if (!surface || surface == server->cursor_surface || !surface->mapped || !surface->current)
        return;
    int own_x = parent_x + (surface->parent ? surface->subsurface_x :
            (surface->layer_surface ? surface->layer_x : 0));
    int own_y = parent_y + (surface->parent ? surface->subsurface_y :
            (surface->layer_surface ? surface->layer_y : 0));
    draw_surface(renderer, surface, own_x, own_y);
    struct compositor_surface *child;
    wl_list_for_each(child, &surface->children, subsurface_link)
        draw_surface_tree(renderer, server, child, own_x, own_y);
}

static bool is_tiny_viewport_underlay(const struct compositor_surface *surface) {
    if (!surface || !surface->current || !surface->viewport_destination_set) return false;
    return (int64_t)surface->current->width * surface->current->height <= 256;
}

#define VULKAN_SCENE_MAX_DRAWS 8

static int vulkan_collect_draws(struct wayland_server *server,
        struct compositor_surface *surface, int parent_x, int parent_y,
        struct trierarch_vulkan_draw *draws, int count, bool *has_shm) {
    if (!surface || surface == server->cursor_surface || surface->is_cursor ||
            !surface->mapped || !surface->current)
        return count;
    struct shm_buffer *buffer = surface->current;
    int scale = surface->buffer_scale > 0 ? surface->buffer_scale : 1;
    int width = surface->viewport_destination_set
            ? surface->viewport_destination_width : buffer->width / scale;
    int height = surface->viewport_destination_set
            ? surface->viewport_destination_height : buffer->height / scale;
    if (width <= 0 || height <= 0)
        return count;
    int own_x = parent_x + (surface->parent ? surface->subsurface_x :
            (surface->layer_surface ? surface->layer_x : 0));
    int own_y = parent_y + (surface->parent ? surface->subsurface_y :
            (surface->layer_surface ? surface->layer_y : 0));
    const bool tiny = (int64_t)buffer->width * buffer->height <= 256;
    if (!tiny) {
        if (buffer->dmabuf && buffer->dmabuf_fd >= 0) {
            if (count < VULKAN_SCENE_MAX_DRAWS) {
                draws[count++] = (struct trierarch_vulkan_draw){
                    .key = buffer->serial,
                    .fd = buffer->dmabuf_fd,
                    .width = buffer->width,
                    .height = buffer->height,
                    .stride = buffer->stride,
                    .offset = buffer->dmabuf_offset,
                    .format = buffer->format,
                    .modifier = buffer->dmabuf_modifier,
                    .dst_x = own_x,
                    .dst_y = own_y,
                    .dst_width = width,
                    .dst_height = height,
                };
            }
        } else if (buffer->data && !buffer->tvg_token) {
            *has_shm = true;
        }
    }
    struct compositor_surface *child;
    wl_list_for_each(child, &surface->children, subsurface_link)
        count = vulkan_collect_draws(server, child, own_x, own_y, draws,
                count, has_shm);
    return count;
}

static void vulkan_scene_extent(struct wayland_server *server, int *out_w, int *out_h) {
    int best_area = 0;
    *out_w = 0;
    *out_h = 0;
    struct compositor_surface *surface;
    wl_list_for_each(surface, &server->surfaces, link) {
        if (surface->parent || surface == server->cursor_surface ||
                surface->is_cursor || !surface->mapped || !surface->current)
            continue;
        struct shm_buffer *buffer = surface->current;
        int scale = surface->buffer_scale > 0 ? surface->buffer_scale : 1;
        int width = surface->viewport_destination_set
                ? surface->viewport_destination_width : buffer->width / scale;
        int height = surface->viewport_destination_set
                ? surface->viewport_destination_height : buffer->height / scale;
        int area = width * height;
        if (width > 0 && height > 0 && area > best_area) {
            best_area = area;
            *out_w = width;
            *out_h = height;
        }
    }
}

bool trierarch_renderer_render(struct renderer_context *renderer,
        struct wayland_server *server) {
    if (!trierarch_renderer_valid(renderer) || !server) return false;
    if (renderer->vulkan) {
        struct compositor_surface *surface;
        struct compositor_surface *best = NULL;
        long long best_area = 0;
        int roots = 0;
        static int64_t last_tree_log;
        int64_t tree_now = monotonic_ns();
        if (tree_now - last_tree_log >= 1000000000LL) {
            last_tree_log = tree_now;
            int logged = 0;
            struct compositor_surface *entry;
            wl_list_for_each(entry, &server->surfaces, link) {
                if (logged++ >= 64) {
                    LOGI("Vulkan surface tree truncated (%d shown)", 64);
                    break;
                }
                int depth = 0;
                struct compositor_surface *parent = entry->parent;
                while (parent && depth < 8) {
                    depth++;
                    parent = parent->parent;
                }
                struct shm_buffer *b = entry->current;
                LOGI("Vulkan surface[%d] %p wl=%p parent=%p depth=%d mapped=%d cursor=%d "
                        "xdg=%d layer=%d sub=%d layer_kind=%u pos=%d,%d subpos=%d,%d "
                        "scale=%d size=%dx%d buf=%p serial=%llu fd=%d bw=%d bh=%d "
                        "stride=%d off=%u fmt=0x%x mod=0x%llx dmabuf=%d vp_dst=%dx%d",
                        logged - 1, (void *)entry, (void *)entry->wl_surface,
                        (void *)entry->parent, depth, entry->mapped ? 1 : 0,
                        entry->is_cursor ? 1 : 0, entry->xdg_toplevel ? 1 : 0,
                        entry->layer_surface ? 1 : 0, entry->parent ? 1 : 0,
                        entry->layer_kind, entry->layer_x, entry->layer_y,
                        entry->subsurface_x, entry->subsurface_y,
                        entry->buffer_scale, entry->width, entry->height,
                        (void *)b, b ? (unsigned long long)b->serial : 0ULL,
                        b ? b->dmabuf_fd : -1, b ? b->width : 0,
                        b ? b->height : 0, b ? b->stride : 0,
                        b ? b->dmabuf_offset : 0,
                        b ? b->format : 0,
                        b ? (unsigned long long)b->dmabuf_modifier : 0ULL,
                        b ? (b->dmabuf ? 1 : 0) : 0,
                        entry->viewport_destination_set ? entry->viewport_destination_width : 0,
                        entry->viewport_destination_set ? entry->viewport_destination_height : 0);
            }
        }
        wl_list_for_each(surface, &server->surfaces, link) {
            if (!surface->mapped || !surface->current ||
                    surface == server->cursor_surface || surface->is_cursor)
                continue;
            roots++;
            struct shm_buffer *candidate = surface->current;
            bool usable = candidate->dmabuf ? candidate->dmabuf_fd >= 0
                    : (candidate->data != NULL && !candidate->tvg_token);
            if (!usable) continue;
            long long area = (long long)candidate->width * candidate->height;
            if (area >= 128 * 128 && area > best_area) {
                best = surface;
                best_area = area;
            }
        }
        struct trierarch_vulkan_draw scene_draws[VULKAN_SCENE_MAX_DRAWS];
        int scene_count = 0;
        bool scene_has_shm = false;
        wl_list_for_each(surface, &server->surfaces, link) {
            if (surface->parent || surface == server->cursor_surface) continue;
            scene_count = vulkan_collect_draws(server, surface, 0, 0,
                    scene_draws, scene_count, &scene_has_shm);
        }
        int scene_width = 0, scene_height = 0;
        vulkan_scene_extent(server, &scene_width, &scene_height);
        if (scene_width <= 0 || scene_height <= 0) {
            for (int i = 0; i < scene_count; i++) {
                int right = scene_draws[i].dst_x + scene_draws[i].dst_width;
                int bottom = scene_draws[i].dst_y + scene_draws[i].dst_height;
                if (right > scene_width) scene_width = right;
                if (bottom > scene_height) scene_height = bottom;
            }
        }
        const bool use_scene = scene_count > 0 && !scene_has_shm &&
                scene_width > 0 && scene_height > 0;
        struct shm_buffer *buffer = best ? best->current : NULL;
        struct trierarch_vulkan_draw cursor_draw;
        bool cursor_set = false;
        if (server->cursor_visible && server->cursor_surface &&
                server->cursor_surface->current &&
                server->cursor_surface->current->dmabuf &&
                server->cursor_surface->current->dmabuf_fd >= 0) {
            struct shm_buffer *cursor = server->cursor_surface->current;
            int scale = server->cursor_surface->buffer_scale > 0
                    ? server->cursor_surface->buffer_scale : 1;
            cursor_draw = (struct trierarch_vulkan_draw){
                .fd = cursor->dmabuf_fd,
                .width = cursor->width,
                .height = cursor->height,
                .stride = cursor->stride,
                .offset = cursor->dmabuf_offset,
                .format = cursor->format,
                .modifier = cursor->dmabuf_modifier,
                .dst_x = (int)(wl_fixed_to_double(server->pointer_x) -
                        server->cursor_hotspot_x / scale),
                .dst_y = (int)(wl_fixed_to_double(server->pointer_y) -
                        server->cursor_hotspot_y / scale),
                .dst_width = cursor->width / scale,
                .dst_height = cursor->height / scale,
            };
            cursor_set = true;
        }
        if (!best && !use_scene) {
            static int64_t last_empty_log;
            int64_t now = monotonic_ns();
            if (now - last_empty_log >= 1000000000LL) {
                last_empty_log = now;
                LOGI("Vulkan scene has no usable surface: roots=%d draws=%d shm=%d",
                        roots, scene_count, scene_has_shm ? 1 : 0);
            }
            return false;
        }
        static int64_t last_scene_log;
        int64_t now = monotonic_ns();
        if (now - last_scene_log >= 1000000000LL) {
            last_scene_log = now;
            if (use_scene) {
                LOGI("Vulkan scene draws=%d extent=%dx%d shm=%d best=%p",
                        scene_count, scene_width, scene_height,
                        scene_has_shm ? 1 : 0, (void *)best);
                for (int i = 0; i < scene_count; i++)
                    LOGI("Vulkan scene draw[%d] key=%llu dst=%d,%d %dx%d src=%dx%d",
                            i, scene_draws[i].key, scene_draws[i].dst_x,
                            scene_draws[i].dst_y, scene_draws[i].dst_width,
                            scene_draws[i].dst_height, scene_draws[i].width,
                            scene_draws[i].height);
            } else if (buffer) {
                LOGI("Vulkan scene surface=%p xdg=%d layer=%d sub=%d size=%dx%d roots=%d shm=%d",
                        (void *)best, best->xdg_toplevel ? 1 : 0,
                        best->layer_surface ? 1 : 0, best->parent ? 1 : 0,
                        buffer->width, buffer->height, roots, buffer->dmabuf ? 0 : 1);
            }
        }
        trierarch_surface_latch_frame_callbacks(server);
        trierarch_vulkan_presenter_set_cursor(
                (trierarch_vulkan_presenter *)renderer->vulkan,
                cursor_set ? &cursor_draw : NULL);
        int result;
        if (use_scene) {
            result = trierarch_vulkan_presenter_present_scene(
                    (trierarch_vulkan_presenter *)renderer->vulkan,
                    scene_draws, scene_count, scene_width, scene_height,
                    cursor_set ? &cursor_draw : NULL);
        } else if (buffer->dmabuf) {
            result = trierarch_vulkan_presenter_present_keyed(
                    (trierarch_vulkan_presenter *)renderer->vulkan,
                    buffer->serial, buffer->dmabuf_fd, buffer->width,
                    buffer->height, buffer->stride, buffer->dmabuf_offset,
                    buffer->format, buffer->dmabuf_modifier);
        } else {
            result = trierarch_vulkan_presenter_present_shm(
                    (trierarch_vulkan_presenter *)renderer->vulkan,
                    buffer->data, buffer->width, buffer->height, buffer->stride,
                    buffer->format);
        }
        if (result == 0) {
            /* The Vulkan presenter already waited for the queue, so every
             * buffer replaced by the commits dispatched for this frame can be
             * returned to its client now.  The release timer paces the actual
             * wl_buffer.release at the output cadence. */
            trierarch_wayland_release_deferred(server);
            trierarch_wayland_frame_presented(server,
                    (uint32_t)(monotonic_ns() / 1000000ULL));
            server->perf_render_count++;
            return true;
        }
        /* A failed present must not pin the previous generation forever.  The
         * queue is idle after this wait, so drain it instead of growing the
         * backlog while the frame is retried. */
        trierarch_vulkan_presenter_wait_idle(
                (trierarch_vulkan_presenter *)renderer->vulkan);
        trierarch_flush_deferred_releases();
        trierarch_surface_requeue_frame_callbacks(server);
        return false;
    }
    renderer->frame_sequence++;
    if (renderer->frame_sequence > 1 && !renderer->frame_fence_created)
        server->diag_fence_missing++;
    /* Keep at most one host frame in flight.  This is deliberately a fence
     * wait, not glFinish: surface commits can release their previous buffer
     * only after this GPU completion point has been reached. */
    trierarch_renderer_wait_idle(renderer);
    /* The previous frame's EGL fence has been waited above.  Now it is safe
     * to release both the guest buffers replaced by commits and the vtest
     * AHBs drawn in that frame. */
    tvg_scanout_begin_frame();
    trierarch_wayland_release_deferred(server);
    uint64_t render_started_ns = monotonic_ns();
    bool surface_updated = renderer->observed_surface_commits != server->perf_surface_commit_generation
            || renderer->observed_surface_damage != server->perf_surface_damage_generation;
    if (surface_updated) {
        static unsigned int trace_count;
        if (trace_count++ < 3)
            LOGI("dmabuf trace stage=render surface-commit-generation=%llu",
                    (unsigned long long)server->perf_surface_commit_generation);
    }
    renderer->observed_surface_commits = server->perf_surface_commit_generation;
    renderer->observed_surface_damage = server->perf_surface_damage_generation;
    if (!eglMakeCurrent(renderer->display, renderer->surface, renderer->surface, renderer->context))
        return false;
    eglQuerySurface(renderer->display, renderer->surface, EGL_WIDTH, &renderer->width);
    eglQuerySurface(renderer->display, renderer->surface, EGL_HEIGHT, &renderer->height);
    glViewport(0, 0, renderer->width, renderer->height);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    struct compositor_surface *surface;
    /* Plasma uses tiny viewport-scaled root buffers during startup. Draw those
     * first so they cannot cover the actual desktop surface. */
    wl_list_for_each(surface, &server->surfaces, link) {
        if (!surface->parent && surface->layer_surface &&
                surface->layer_kind == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND)
            draw_surface_tree(renderer, server, surface, 0, 0);
    }
    wl_list_for_each(surface, &server->surfaces, link) {
        if (!surface->parent && surface != server->cursor_surface && is_tiny_viewport_underlay(surface))
            draw_surface_tree(renderer, server, surface, 0, 0);
    }
    wl_list_for_each(surface, &server->surfaces, link) {
        if (!surface->parent && surface != server->cursor_surface &&
                (!surface->layer_surface ||
                 surface->layer_kind == ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM) &&
                !is_tiny_viewport_underlay(surface))
            draw_surface_tree(renderer, server, surface, 0, 0);
    }
    wl_list_for_each(surface, &server->surfaces, link) {
        if (!surface->parent && surface->layer_surface &&
                (surface->layer_kind == ZWLR_LAYER_SHELL_V1_LAYER_TOP ||
                 surface->layer_kind == ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY))
            draw_surface_tree(renderer, server, surface, 0, 0);
    }
    if (server->cursor_visible && server->cursor_surface && server->cursor_surface->mapped &&
            server->cursor_surface->current) {
        int cursor_x = (int)wl_fixed_to_int(server->pointer_x) - server->cursor_hotspot_x;
        int cursor_y = (int)wl_fixed_to_int(server->pointer_y) - server->cursor_hotspot_y;
        draw_surface(renderer, server->cursor_surface, cursor_x, cursor_y);
    }
    glDisable(GL_BLEND);
    /* Snapshot only callbacks for surfaces that participated in this output
     * composition.  A later commit cannot be completed by this swap. */
    trierarch_surface_latch_frame_callbacks(server);
    uint64_t swap_started_ns = monotonic_ns();
    EGLBoolean swapped = eglSwapBuffers(renderer->display, renderer->surface);
    uint64_t render_finished_ns = monotonic_ns();
    if (!swapped) {
        server->diag_swap_fail++;
        if (server->diag_swap_fail <= 8)
            LOGE("swap failed frame=%llu commits=%llu buffer=%p %dx%d fmt=0x%x mod=0x%llx path=%s",
                    (unsigned long long)renderer->frame_sequence,
                    (unsigned long long)server->perf_surface_commit_generation,
                    renderer->last_buffer, renderer->last_width, renderer->last_height,
                    renderer->last_format, (unsigned long long)renderer->last_modifier,
                    renderer->last_import_path ? renderer->last_import_path : "none");
        renderer->frame_fence_created = false;
        trierarch_surface_requeue_frame_callbacks(server);
        eglMakeCurrent(renderer->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        return false;
    }
    server->diag_swap_ok++;
    bool frame_fence_created = false;
    if (renderer->create_sync && renderer->destroy_sync && renderer->client_wait_sync) {
        const EGLint attributes[] = { EGL_NONE };
        renderer->frame_fence = renderer->create_sync(renderer->display,
                EGL_SYNC_FENCE_KHR, attributes);
        if (renderer->frame_fence == EGL_NO_SYNC_KHR) {
            LOGE("EGL frame fence creation failed");
        } else {
            glFlush();
            frame_fence_created = true;
            server->diag_fence_created++;
        }
    }
    if (!frame_fence_created) {
        /* No fence to defer against; complete the GPU work now. */
        glFinish();
    }
    renderer->frame_fence_created = true;
    /* Move this frame's tokens to inflight.  They are released at the start of
     * the next frame, after its fence wait. */
    tvg_scanout_end_frame();
    trierarch_wayland_frame_presented(server, (uint32_t)(render_finished_ns / 1000000ULL));
    eglMakeCurrent(renderer->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    server->perf_render_count++;
    if (!surface_updated) server->perf_render_without_surface_update++;
    server->perf_render_ns += render_finished_ns - render_started_ns;
    server->perf_swap_ns += render_finished_ns - swap_started_ns;
    record_max(render_finished_ns - render_started_ns, &server->perf_render_max_ns);
    record_max(render_finished_ns - swap_started_ns, &server->perf_swap_max_ns);
    return true;
}
