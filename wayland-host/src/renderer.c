#include "renderer.h"
#include "server_internal.h"
#include "gpu_probe.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <android/log.h>
#include <errno.h>
#include <poll.h>
#include <unistd.h>
#include <stdlib.h>
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

#define DRM_FORMAT_MOD_INVALID 0x00ffffffffffffffULL

typedef EGLBoolean (*bind_wayland_display_fn)(EGLDisplay, void *);
typedef EGLBoolean (*query_wayland_buffer_fn)(EGLDisplay, void *, EGLint, EGLint *);
typedef EGLImageKHR (*create_image_fn)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
typedef EGLBoolean (*destroy_image_fn)(EGLDisplay, EGLImageKHR);
typedef void (*image_target_fn)(GLenum, GLeglImageOES);
typedef EGLClientBuffer (*native_client_buffer_fn)(void *);
typedef EGLSyncKHR (*create_sync_fn)(EGLDisplay, EGLenum, const EGLint *);
typedef EGLBoolean (*destroy_sync_fn)(EGLDisplay, EGLSyncKHR);
typedef EGLint (*dup_native_fence_fd_fn)(EGLDisplay, EGLSyncKHR);

struct renderer_context {
    EGLDisplay display;
    EGLSurface surface;
    EGLContext context;
    ANativeWindow *window;
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
    dup_native_fence_fd_fn dup_native_fence_fd;
    bool dmabuf_import_supported;
    uint64_t observed_surface_commits;
    uint64_t observed_surface_damage;
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
    renderer->dup_native_fence_fd = (dup_native_fence_fd_fn)eglGetProcAddress(
            "eglDupNativeFenceFDANDROID");
    const char *extensions = eglQueryString(renderer->display, EGL_EXTENSIONS);
    renderer->dmabuf_import_supported = extensions &&
            strstr(extensions, "EGL_EXT_image_dma_buf_import") &&
            renderer->create_image && renderer->destroy_image && renderer->image_target;
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
    LOGI("EGL dma-buf import %s", renderer->dmabuf_import_supported ? "available" : "unavailable");
    eglQuerySurface(renderer->display, renderer->surface, EGL_WIDTH, &renderer->width);
    eglQuerySurface(renderer->display, renderer->surface, EGL_HEIGHT, &renderer->height);
    renderer->background_program = make_program(background_source);
    renderer->texture_program = make_program(texture_source);
    glGenTextures(1, &renderer->texture);
    renderer->valid = renderer->background_program && renderer->texture_program && renderer->texture;
    eglMakeCurrent(renderer->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (!renderer->valid) goto fail;
    return renderer;

fail:
    trierarch_renderer_destroy(renderer);
    return NULL;
}

void trierarch_renderer_destroy(struct renderer_context *renderer) {
    if (!renderer) return;
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

bool trierarch_renderer_valid(const struct renderer_context *renderer) {
    return renderer && renderer->valid;
}

static void draw_surface(struct renderer_context *renderer,
        struct compositor_surface *surface, int x, int y) {
    struct shm_buffer *buffer = surface->current;
    if (!buffer) return;
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
            swizzle = 0.0f;
            opaque = 0.0f;
        }
    }
    if (image == EGL_NO_IMAGE_KHR && buffer->egl_buffer &&
            renderer->create_image && renderer->image_target) {
        image = renderer->create_image(renderer->display, renderer->context,
                EGL_WAYLAND_BUFFER_WL, (EGLClientBuffer)buffer->egl_resource, NULL);
        if (image != EGL_NO_IMAGE_KHR) { renderer->image_target(GL_TEXTURE_2D, image); swizzle = 0.0f; opaque = 0.0f; }
    }
    if (image == EGL_NO_IMAGE_KHR && buffer->dmabuf && buffer->dmabuf_fd >= 0 &&
            renderer->dmabuf_import_supported) {
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
        if (image == EGL_NO_IMAGE_KHR && buffer->dmabuf_modifier != DRM_FORMAT_MOD_INVALID) {
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
        }
        if (image != EGL_NO_IMAGE_KHR) {
            glBindTexture(GL_TEXTURE_2D, renderer->texture);
            renderer->image_target(GL_TEXTURE_2D, image);
            if (glGetError() == GL_NO_ERROR) {
                swizzle = 0.0f;
                opaque = buffer->format == 0x34325258u || buffer->format == 0x34324258u ? 1.0f : 0.0f;
                LOGI("dma-buf EGL import: %dx%d fmt=0x%x modifier=0x%llx",
                        buffer->width, buffer->height, buffer->format,
                        (unsigned long long)buffer->dmabuf_modifier);
            } else {
                renderer->destroy_image(renderer->display, image);
                image = EGL_NO_IMAGE_KHR;
            }
        } else {
            LOGI("dma-buf EGL import failed: %dx%d fmt=0x%x modifier=0x%llx; using CPU fallback",
                    buffer->width, buffer->height, buffer->format,
                    (unsigned long long)buffer->dmabuf_modifier);
        }
    }
    if (image == EGL_NO_IMAGE_KHR && buffer->data) {
        const int packed_stride = buffer->width * 4;
        const void *pixels = buffer->data;
        void *packed = NULL;
        if (buffer->stride != packed_stride) {
            packed = malloc((size_t)packed_stride * (size_t)buffer->height);
            if (!packed) return;
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
        /* XRGB has no alpha channel. ARGB must retain its alpha: cursor images
         * rely on transparent pixels around the visible pointer shape. */
        opaque = buffer->format == WL_SHM_FORMAT_XRGB8888 ? 1.0f : 0.0f;
    }
    if (image == EGL_NO_IMAGE_KHR && !buffer->data) {
        static unsigned int unavailable_buffer_count;
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
    int own_x = parent_x + (surface->parent ? surface->subsurface_x : 0);
    int own_y = parent_y + (surface->parent ? surface->subsurface_y : 0);
    draw_surface(renderer, surface, own_x, own_y);
    struct compositor_surface *child;
    wl_list_for_each(child, &surface->children, subsurface_link)
        draw_surface_tree(renderer, server, child, own_x, own_y);
}

/* This is intentionally not a Wayland client buffer path.  A test client
 * sends one exported guest buffer through gpu-probe.sock; the active renderer
 * imports it in its own EGL context and overlays it once. */
static int draw_gpu_probe(struct renderer_context *renderer,
        struct wayland_server *server, int *result_client_fd) {
    *result_client_fd = -1;
    if (!server->gpu_probe) return 0;
    struct trierarch_gpu_probe_buffer buffer;
    int buffer_fd = -1;
    int client_fd = -1;
    if (!trierarch_gpu_probe_take(server->gpu_probe, &buffer, &buffer_fd, &client_fd)) return 0;
    if (!renderer->dmabuf_import_supported) {
        trierarch_gpu_probe_report(client_fd, TRIERARCH_GPU_PROBE_IMPORT_UNAVAILABLE, EGL_SUCCESS, -1);
        close(buffer_fd);
        return 0;
    }
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    const EGLint basic_attributes[] = {
        EGL_WIDTH, (EGLint)buffer.width, EGL_HEIGHT, (EGLint)buffer.height,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)buffer.drm_format,
        EGL_DMA_BUF_PLANE0_FD_EXT, buffer_fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)buffer.stride, EGL_NONE,
    };
    image = renderer->create_image(renderer->display, EGL_NO_CONTEXT,
            EGL_LINUX_DMA_BUF_EXT, NULL, basic_attributes);
    if (image == EGL_NO_IMAGE_KHR && buffer.modifier != DRM_FORMAT_MOD_INVALID) {
        const EGLint modifier_attributes[] = {
            EGL_WIDTH, (EGLint)buffer.width, EGL_HEIGHT, (EGLint)buffer.height,
            EGL_LINUX_DRM_FOURCC_EXT, (EGLint)buffer.drm_format,
            EGL_DMA_BUF_PLANE0_FD_EXT, buffer_fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)buffer.stride,
            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(uint32_t)buffer.modifier,
            EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(uint32_t)(buffer.modifier >> 32),
            EGL_NONE,
        };
        image = renderer->create_image(renderer->display, EGL_NO_CONTEXT,
                EGL_LINUX_DMA_BUF_EXT, NULL, modifier_attributes);
    }
    close(buffer_fd);
    if (image == EGL_NO_IMAGE_KHR) {
        EGLint error = eglGetError();
        LOGE("gpu probe EGL import failed: format=0x%x modifier=0x%llx error=0x%x",
                buffer.drm_format, (unsigned long long)buffer.modifier, error);
        trierarch_gpu_probe_report(client_fd, TRIERARCH_GPU_PROBE_IMPORT_FAILED, error, -1);
        return 0;
    }
    int width = (int)buffer.width;
    int height = (int)buffer.height;
    const int max_edge = renderer->width < renderer->height ? renderer->width / 3 : renderer->height / 3;
    if (width > max_edge || height > max_edge) {
        float scale = (float)max_edge / (float)(width > height ? width : height);
        width = (int)(width * scale);
        height = (int)(height * scale);
    }
    const int x = 24;
    const int y = 24;
    const float left = 2.0f * x / renderer->width - 1.0f;
    const float right = 2.0f * (x + width) / renderer->width - 1.0f;
    const float top = 1.0f - 2.0f * y / renderer->height;
    const float bottom = 1.0f - 2.0f * (y + height) / renderer->height;
    const GLfloat vertices[] = { left,bottom,0,1, right,bottom,1,1,
            left,top,0,0, right,top,1,0 };
    glUseProgram(renderer->texture_program);
    glBindTexture(GL_TEXTURE_2D, renderer->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    renderer->image_target(GL_TEXTURE_2D, image);
    glUniform1f(glGetUniformLocation(renderer->texture_program, "swizzle"), 0.0f);
    glUniform1f(glGetUniformLocation(renderer->texture_program, "opaque"), 1.0f);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), vertices);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), vertices + 2);
    glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0); glDisableVertexAttribArray(1);
    GLenum gl_error = glGetError();
    renderer->destroy_image(renderer->display, image);
    if (gl_error != GL_NO_ERROR) {
        LOGE("gpu probe texture draw failed: 0x%x", gl_error);
        trierarch_gpu_probe_report(client_fd, TRIERARCH_GPU_PROBE_DRAW_FAILED, EGL_SUCCESS, -1);
        return 0;
    }
    LOGI("gpu probe imported and drew guest buffer; awaiting Android swap");
    *result_client_fd = client_fd;
    return 1;
}

static bool wait_native_fence(int fence_fd) {
    if (fence_fd < 0) return false;
    struct pollfd descriptor = { .fd = fence_fd, .events = POLLIN };
    int result;
    do { result = poll(&descriptor, 1, 3000); } while (result < 0 && errno == EINTR);
    close(fence_fd);
    return result == 1 && !(descriptor.revents & (POLLERR | POLLNVAL));
}

static int create_native_fence(struct renderer_context *renderer) {
    if (!renderer->create_sync || !renderer->destroy_sync || !renderer->dup_native_fence_fd)
        return -1;
    EGLSyncKHR sync = renderer->create_sync(renderer->display, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
    if (sync == EGL_NO_SYNC_KHR) return -1;
    /* Queue all prior sampling commands before duplicating its sync_file FD. */
    glFlush();
    EGLint fence_fd = renderer->dup_native_fence_fd(renderer->display, sync);
    renderer->destroy_sync(renderer->display, sync);
    return fence_fd >= 0 ? fence_fd : -1;
}

/* Reverse feasibility probe: Android allocates this AHardwareBuffer, the guest
 * renders through its exported pixel FD, then Android waits the guest release
 * fence before sampling. The reply returns a host read-complete fence. */
static int draw_host_gpu_probe(struct renderer_context *renderer,
        struct wayland_server *server, int *result_client_fd, int *host_fence_fd) {
    *result_client_fd = -1;
    *host_fence_fd = -1;
    if (!server->gpu_probe || !renderer->create_image || !renderer->destroy_image ||
            !renderer->image_target || !renderer->native_client_buffer) return 0;
    AHardwareBuffer *buffer = NULL;
    int client_fd = -1;
    int guest_fence_fd = -1;
    if (!trierarch_gpu_probe_take_host_buffer(server->gpu_probe, &buffer, &client_fd, &guest_fence_fd)) return 0;
    if (!wait_native_fence(guest_fence_fd)) {
        LOGE("reverse gpu probe guest release fence wait failed: %s", strerror(errno));
        AHardwareBuffer_release(buffer);
        trierarch_gpu_probe_report(client_fd, TRIERARCH_GPU_PROBE_DRAW_FAILED, EGL_SUCCESS, -1);
        return 0;
    }
    EGLClientBuffer native_buffer = renderer->native_client_buffer(buffer);
    const EGLint attributes[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
    EGLImageKHR image = native_buffer ? renderer->create_image(renderer->display, EGL_NO_CONTEXT,
            EGL_NATIVE_BUFFER_ANDROID, native_buffer, attributes) : EGL_NO_IMAGE_KHR;
    if (image == EGL_NO_IMAGE_KHR) {
        EGLint error = eglGetError();
        LOGE("reverse gpu probe Android buffer image creation failed: 0x%x", error);
        AHardwareBuffer_release(buffer);
        trierarch_gpu_probe_report(client_fd, TRIERARCH_GPU_PROBE_IMPORT_FAILED, error, -1);
        return 0;
    }
    const int width = renderer->width / 4;
    const int height = renderer->height / 4;
    const int x = renderer->width - width - 24;
    const int y = 24;
    const float left = 2.0f * x / renderer->width - 1.0f;
    const float right = 2.0f * (x + width) / renderer->width - 1.0f;
    const float top = 1.0f - 2.0f * y / renderer->height;
    const float bottom = 1.0f - 2.0f * (y + height) / renderer->height;
    const GLfloat vertices[] = { left,bottom,0,1, right,bottom,1,1,
            left,top,0,0, right,top,1,0 };
    glUseProgram(renderer->texture_program);
    glBindTexture(GL_TEXTURE_2D, renderer->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    renderer->image_target(GL_TEXTURE_2D, image);
    glUniform1f(glGetUniformLocation(renderer->texture_program, "swizzle"), 0.0f);
    glUniform1f(glGetUniformLocation(renderer->texture_program, "opaque"), 1.0f);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), vertices);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), vertices + 2);
    glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0); glDisableVertexAttribArray(1);
    GLenum gl_error = glGetError();
    renderer->destroy_image(renderer->display, image);
    AHardwareBuffer_release(buffer);
    if (gl_error != GL_NO_ERROR) {
        LOGE("reverse gpu probe Android buffer sampling failed: 0x%x", gl_error);
        trierarch_gpu_probe_report(client_fd, TRIERARCH_GPU_PROBE_DRAW_FAILED, EGL_SUCCESS, -1);
        return 0;
    }
    int fence_fd = create_native_fence(renderer);
    if (fence_fd < 0) {
        LOGE("reverse gpu probe could not export host read-complete fence");
        trierarch_gpu_probe_report(client_fd, TRIERARCH_GPU_PROBE_IMPORT_UNAVAILABLE, eglGetError(), -1);
        return 0;
    }
    LOGI("reverse gpu probe waited guest fence and sampled Android buffer; awaiting Android swap");
    *result_client_fd = client_fd;
    *host_fence_fd = fence_fd;
    return 1;
}

static bool is_tiny_viewport_underlay(const struct compositor_surface *surface) {
    if (!surface || !surface->current || !surface->viewport_destination_set) return false;
    return (int64_t)surface->current->width * surface->current->height <= 256;
}

bool trierarch_renderer_render(struct renderer_context *renderer,
        struct wayland_server *server) {
    if (!trierarch_renderer_valid(renderer) || !server) return false;
    uint64_t render_started_ns = monotonic_ns();
    bool surface_updated = renderer->observed_surface_commits != server->perf_surface_commit_generation
            || renderer->observed_surface_damage != server->perf_surface_damage_generation;
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
        if (!surface->parent && surface != server->cursor_surface && is_tiny_viewport_underlay(surface))
            draw_surface_tree(renderer, server, surface, 0, 0);
    }
    wl_list_for_each(surface, &server->surfaces, link) {
        if (!surface->parent && surface != server->cursor_surface && !is_tiny_viewport_underlay(surface))
            draw_surface_tree(renderer, server, surface, 0, 0);
    }
    if (server->cursor_visible && server->cursor_surface && server->cursor_surface->mapped &&
            server->cursor_surface->current) {
        int cursor_x = (int)wl_fixed_to_int(server->pointer_x) - server->cursor_hotspot_x;
        int cursor_y = (int)wl_fixed_to_int(server->pointer_y) - server->cursor_hotspot_y;
        draw_surface(renderer, server->cursor_surface, cursor_x, cursor_y);
    }
    int gpu_probe_client_fd = -1;
    (void)draw_gpu_probe(renderer, server, &gpu_probe_client_fd);
    int host_gpu_probe_client_fd = -1;
    int host_gpu_probe_fence_fd = -1;
    (void)draw_host_gpu_probe(renderer, server, &host_gpu_probe_client_fd, &host_gpu_probe_fence_fd);
    glDisable(GL_BLEND);
    /* Snapshot only callbacks for surfaces that participated in this output
     * composition.  A later commit cannot be completed by this swap. */
    trierarch_surface_latch_frame_callbacks(server);
    uint64_t swap_started_ns = monotonic_ns();
    EGLBoolean swapped = eglSwapBuffers(renderer->display, renderer->surface);
    uint64_t render_finished_ns = monotonic_ns();
    if (!swapped) {
        trierarch_surface_requeue_frame_callbacks(server);
        trierarch_gpu_probe_report(gpu_probe_client_fd, TRIERARCH_GPU_PROBE_DRAW_FAILED,
                eglGetError(), -1);
        trierarch_gpu_probe_report(host_gpu_probe_client_fd, TRIERARCH_GPU_PROBE_DRAW_FAILED,
                eglGetError(), host_gpu_probe_fence_fd);
        eglMakeCurrent(renderer->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        return false;
    }
    trierarch_gpu_probe_report(gpu_probe_client_fd, TRIERARCH_GPU_PROBE_OK, EGL_SUCCESS, -1);
    trierarch_gpu_probe_report(host_gpu_probe_client_fd, TRIERARCH_GPU_PROBE_OK, EGL_SUCCESS,
            host_gpu_probe_fence_fd);
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
