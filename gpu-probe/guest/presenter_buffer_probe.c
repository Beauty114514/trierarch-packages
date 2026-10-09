#define _POSIX_C_SOURCE 200809L

#include "trierarch-adreno-presenter-v1-client-protocol.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#define DRM_FORMAT_ABGR8888 0x34324241u
#ifndef EGL_LINUX_DMA_BUF_EXT
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
#endif
#ifndef EGL_SYNC_NATIVE_FENCE_ANDROID
#define EGL_SYNC_NATIVE_FENCE_ANDROID 0x3144
#endif
#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif

typedef EGLDisplay (*get_platform_display_fn)(EGLenum, void *, const EGLint *);
typedef EGLImageKHR (*create_image_fn)(EGLDisplay, EGLContext, EGLenum,
        EGLClientBuffer, const EGLint *);
typedef EGLBoolean (*destroy_image_fn)(EGLDisplay, EGLImageKHR);
typedef void (*image_target_fn)(GLenum, GLeglImageOES);
typedef EGLSyncKHR (*create_sync_fn)(EGLDisplay, EGLenum, const EGLint *);
typedef EGLBoolean (*destroy_sync_fn)(EGLDisplay, EGLSyncKHR);
typedef EGLint (*dup_fence_fn)(EGLDisplay, EGLSyncKHR);

struct probe {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct trierarch_adreno_presenter_v1 *presenter;
    struct trierarch_adreno_buffer_v1 *request;
    struct wl_surface *surface;
    struct wl_buffer *buffer;
    struct wl_callback *frame;
    uint32_t compositor_version;
    uint32_t capabilities;
    uint32_t stride;
    uint64_t modifier;
    int dma_buf_fd;
    bool ready;
    bool failed;
    bool frame_done;
    bool buffer_released;
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
        const char *interface, uint32_t version) {
    struct probe *probe = data;
    if (!strcmp(interface, wl_compositor_interface.name)) {
        probe->compositor_version = version < 4 ? version : 4;
        probe->compositor = wl_registry_bind(registry, name, &wl_compositor_interface,
                probe->compositor_version);
    } else if (!strcmp(interface, trierarch_adreno_presenter_v1_interface.name)) {
        probe->presenter = wl_registry_bind(registry, name,
                &trierarch_adreno_presenter_v1_interface, 1);
    }
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

static void capabilities(void *data, struct trierarch_adreno_presenter_v1 *presenter,
        uint32_t flags) {
    (void)presenter;
    ((struct probe *)data)->capabilities = flags;
}

static const struct trierarch_adreno_presenter_v1_listener presenter_listener = {
    .capabilities = capabilities,
};

static void buffer_ready(void *data, struct trierarch_adreno_buffer_v1 *request,
        struct wl_buffer *buffer, int32_t dma_buf_fd, uint32_t stride,
        uint32_t offset, uint32_t modifier_hi, uint32_t modifier_lo, uint32_t slot) {
    (void)request; (void)slot;
    struct probe *probe = data;
    probe->buffer = buffer;
    probe->dma_buf_fd = dma_buf_fd;
    probe->stride = stride;
    probe->modifier = ((uint64_t)modifier_hi << 32) | modifier_lo;
    probe->ready = offset == 0;
    probe->failed = offset != 0;
}

static void buffer_failed(void *data, struct trierarch_adreno_buffer_v1 *request,
        uint32_t reason) {
    (void)request;
    fprintf(stderr, "presenter allocation failed: reason=%u\n", reason);
    ((struct probe *)data)->failed = true;
}

static const struct trierarch_adreno_buffer_v1_listener request_listener = {
    .ready = buffer_ready,
    .failed = buffer_failed,
};

static void frame_done(void *data, struct wl_callback *callback, uint32_t time) {
    (void)time;
    struct probe *probe = data;
    probe->frame_done = true;
    probe->frame = NULL;
    wl_callback_destroy(callback);
}

static const struct wl_callback_listener frame_listener = { .done = frame_done };

static void buffer_release(void *data, struct wl_buffer *buffer) {
    (void)buffer;
    ((struct probe *)data)->buffer_released = true;
}

static const struct wl_buffer_listener buffer_listener = { .release = buffer_release };

static bool wait_for(struct probe *probe, const bool *condition, int seconds) {
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (!*condition && !probe->failed) {
        if (wl_display_dispatch_pending(probe->display) < 0) return false;
        if (*condition || probe->failed) break;
        if (wl_display_flush(probe->display) < 0 && errno != EAGAIN) return false;
        struct pollfd fd = { .fd = wl_display_get_fd(probe->display), .events = POLLIN };
        int result;
        do { result = poll(&fd, 1, 100); } while (result < 0 && errno == EINTR);
        if (result < 0 || (result > 0 && wl_display_dispatch(probe->display) < 0))
            return false;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - start.tv_sec >= seconds) return false;
    }
    return *condition && !probe->failed;
}

static bool has_extension(const char *extensions, const char *name) {
    if (!extensions) return false;
    size_t length = strlen(name);
    for (const char *entry = extensions; (entry = strstr(entry, name)); ++entry) {
        if ((entry == extensions || entry[-1] == ' ') &&
                (entry[length] == ' ' || entry[length] == '\0')) return true;
    }
    return false;
}

static bool parse_dimension(const char *value, uint32_t *out) {
    char *end = NULL;
    errno = 0;
    unsigned long number = strtoul(value, &end, 10);
    if (errno || !end || *end || number == 0 || number > 8192) return false;
    *out = (uint32_t)number;
    return true;
}

static void disconnect_probe(struct probe *probe) {
    if (probe->frame) wl_callback_destroy(probe->frame);
    if (probe->buffer) wl_buffer_destroy(probe->buffer);
    if (probe->request) trierarch_adreno_buffer_v1_destroy(probe->request);
    if (probe->surface) wl_surface_destroy(probe->surface);
    if (probe->presenter) trierarch_adreno_presenter_v1_destroy(probe->presenter);
    if (probe->compositor) wl_compositor_destroy(probe->compositor);
    if (probe->registry) wl_registry_destroy(probe->registry);
    if (probe->display) wl_display_disconnect(probe->display);
    if (probe->dma_buf_fd >= 0) close(probe->dma_buf_fd);
}

static int run(uint32_t width, uint32_t height) {
    int status = 1;
    struct probe probe = { .dma_buf_fd = -1 };
    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface pbuffer = EGL_NO_SURFACE;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint texture = 0, framebuffer = 0;
    destroy_image_fn destroy_image = NULL;

    probe.display = wl_display_connect(NULL);
    if (!probe.display) { fputs("parent Wayland socket unavailable\n", stderr); goto done; }
    probe.registry = wl_display_get_registry(probe.display);
    wl_registry_add_listener(probe.registry, &registry_listener, &probe);
    if (wl_display_roundtrip(probe.display) < 0 || !probe.compositor || !probe.presenter) {
        fputs("parent compositor lacks trierarch_adreno_presenter_v1\n", stderr);
        goto done;
    }
    trierarch_adreno_presenter_v1_add_listener(probe.presenter, &presenter_listener, &probe);
    if (wl_display_roundtrip(probe.display) < 0 ||
            !(probe.capabilities & TRIERARCH_ADRENO_PRESENTER_V1_CAPABILITY_HOST_BUFFER) ||
            !(probe.capabilities & TRIERARCH_ADRENO_PRESENTER_V1_CAPABILITY_EXPLICIT_SYNC)) {
        fputs("presenter lacks host-buffer or explicit-sync support\n", stderr);
        goto done;
    }
    probe.request = trierarch_adreno_presenter_v1_create_buffer(probe.presenter,
            (int32_t)width, (int32_t)height, DRM_FORMAT_ABGR8888, 0);
    if (!probe.request) goto done;
    trierarch_adreno_buffer_v1_add_listener(probe.request, &request_listener, &probe);
    if (!wait_for(&probe, &probe.ready, 5) || probe.dma_buf_fd < 0 ||
            probe.modifier != 0 || probe.stride < width * 4u) {
        fprintf(stderr, "presenter buffer unavailable: stride=%u modifier=0x%llx\n",
                probe.stride, (unsigned long long)probe.modifier);
        goto done;
    }

    get_platform_display_fn get_display =
            (get_platform_display_fn)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!get_display) { fputs("surfaceless EGL entry point unavailable\n", stderr); goto done; }
    egl_display = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (egl_display == EGL_NO_DISPLAY || !eglInitialize(egl_display, NULL, NULL)) goto done;
    const char *extensions = eglQueryString(egl_display, EGL_EXTENSIONS);
    if (!has_extension(extensions, "EGL_EXT_image_dma_buf_import") ||
            !has_extension(extensions, "EGL_ANDROID_native_fence_sync")) {
        fputs("guest EGL lacks dma-buf import or native fences\n", stderr);
        goto done;
    }
    EGLConfig config;
    EGLint count = 0;
    const EGLint config_attributes[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
    const EGLint context_attributes[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    const EGLint surface_attributes[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    if (!eglChooseConfig(egl_display, config_attributes, &config, 1, &count) || !count)
        goto done;
    context = eglCreateContext(egl_display, config, EGL_NO_CONTEXT, context_attributes);
    pbuffer = eglCreatePbufferSurface(egl_display, config, surface_attributes);
    if (context == EGL_NO_CONTEXT || pbuffer == EGL_NO_SURFACE ||
            !eglMakeCurrent(egl_display, pbuffer, pbuffer, context)) goto done;
    printf("guest renderer: %s\n", glGetString(GL_RENDERER));

    create_image_fn create_image = (create_image_fn)eglGetProcAddress("eglCreateImageKHR");
    destroy_image = (destroy_image_fn)eglGetProcAddress("eglDestroyImageKHR");
    image_target_fn image_target = (image_target_fn)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    create_sync_fn create_sync = (create_sync_fn)eglGetProcAddress("eglCreateSyncKHR");
    destroy_sync_fn destroy_sync = (destroy_sync_fn)eglGetProcAddress("eglDestroySyncKHR");
    dup_fence_fn dup_fence = (dup_fence_fn)eglGetProcAddress("eglDupNativeFenceFDANDROID");
    if (!create_image || !destroy_image || !image_target || !create_sync || !destroy_sync ||
            !dup_fence) goto done;
    const EGLint image_attributes[] = { EGL_WIDTH, (EGLint)width,
            EGL_HEIGHT, (EGLint)height, EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ABGR8888,
            EGL_DMA_BUF_PLANE0_FD_EXT, probe.dma_buf_fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)probe.stride, EGL_NONE };
    image = create_image(egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
            NULL, image_attributes);
    if (image == EGL_NO_IMAGE_KHR) {
        fprintf(stderr, "guest EGL dma-buf import failed: 0x%x\n", eglGetError());
        goto done;
    }
    close(probe.dma_buf_fd);
    probe.dma_buf_fd = -1;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    image_target(GL_TEXTURE_2D, image);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
            texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fputs("guest EGL image is not a renderable framebuffer\n", stderr);
        goto done;
    }
    glViewport(0, 0, (GLsizei)width, (GLsizei)height);
    glClearColor(0.15f, 0.62f, 0.85f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    uint8_t pixel[4] = {0};
    glReadPixels((GLint)(width / 2), (GLint)(height / 2), 1, 1,
            GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (glGetError() != GL_NO_ERROR || pixel[0] < 35 || pixel[0] > 42 ||
            pixel[1] < 155 || pixel[1] > 162 || pixel[2] < 213 || pixel[2] > 221 ||
            pixel[3] != 255) {
        fprintf(stderr, "guest render/readback failed: RGBA=%u,%u,%u,%u\n",
                pixel[0], pixel[1], pixel[2], pixel[3]);
        goto done;
    }
    EGLSyncKHR fence = create_sync(egl_display, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
    if (fence == EGL_NO_SYNC_KHR) goto done;
    glFlush();
    int fence_fd = dup_fence(egl_display, fence);
    destroy_sync(egl_display, fence);
    if (fence_fd < 0) goto done;

    probe.surface = wl_compositor_create_surface(probe.compositor);
    if (!probe.surface) { close(fence_fd); goto done; }
    wl_buffer_add_listener(probe.buffer, &buffer_listener, &probe);
    int request_fd = dup(fence_fd);
    if (request_fd < 0) { close(fence_fd); goto done; }
    /* The Wayland fd argument owns request_fd. Retain and close our original. */
    trierarch_adreno_buffer_v1_submit(probe.request, request_fd);
    if (wl_display_flush(probe.display) < 0 && errno != EAGAIN) {
        close(fence_fd);
        goto done;
    }
    close(fence_fd);
    probe.frame = wl_surface_frame(probe.surface);
    wl_callback_add_listener(probe.frame, &frame_listener, &probe);
    wl_surface_attach(probe.surface, probe.buffer, 0, 0);
    if (probe.compositor_version >= 4)
        wl_surface_damage_buffer(probe.surface, 0, 0, (int32_t)width, (int32_t)height);
    else
        wl_surface_damage(probe.surface, 0, 0, (int32_t)width, (int32_t)height);
    wl_surface_commit(probe.surface);
    if (!wait_for(&probe, &probe.frame_done, 5)) {
        fputs("host did not present the buffer\n", stderr);
        goto done;
    }
    printf("host presented %ux%u host-owned AHardwareBuffer; stride=%u\n",
            width, height, probe.stride);
    /* Leave the solid-color surface visible briefly for an independent
     * Android screenshot. This is a one-shot probe, not a frame queue. */
    sleep(8);
    wl_surface_attach(probe.surface, NULL, 0, 0);
    wl_surface_commit(probe.surface);
    if (!wait_for(&probe, &probe.buffer_released, 5)) {
        fputs("host did not release the buffer\n", stderr);
        goto done;
    }
    status = 0;

done:
    if (egl_display != EGL_NO_DISPLAY && context != EGL_NO_CONTEXT &&
            pbuffer != EGL_NO_SURFACE)
        eglMakeCurrent(egl_display, pbuffer, pbuffer, context);
    if (framebuffer) glDeleteFramebuffers(1, &framebuffer);
    if (texture) glDeleteTextures(1, &texture);
    if (image != EGL_NO_IMAGE_KHR && destroy_image) destroy_image(egl_display, image);
    if (egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (pbuffer != EGL_NO_SURFACE) eglDestroySurface(egl_display, pbuffer);
        if (context != EGL_NO_CONTEXT) eglDestroyContext(egl_display, context);
        eglTerminate(egl_display);
    }
    disconnect_probe(&probe);
    return status;
}

int main(int argc, char **argv) {
    uint32_t width, height;
    if (argc != 3 || !parse_dimension(argv[1], &width) ||
            !parse_dimension(argv[2], &height)) {
        fprintf(stderr, "usage: %s WIDTH HEIGHT (1..8192)\n", argv[0]);
        return 64;
    }
    return run(width, height);
}
