#define _POSIX_C_SOURCE 200809L
#include "../protocol.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <wayland-client.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef EGL_LINUX_DMA_BUF_EXT
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#endif
#ifndef EGL_LINUX_DRM_FOURCC_EXT
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#endif

typedef EGLBoolean (*egl_export_dmabuf_query_mesa_fn)(EGLDisplay, EGLImageKHR,
        int *, int *, EGLuint64KHR *);
typedef EGLBoolean (*egl_export_dmabuf_mesa_fn)(EGLDisplay, EGLImageKHR,
        int *, EGLint *, EGLint *);
typedef EGLImageKHR (*egl_create_image_khr_fn)(EGLDisplay, EGLContext, EGLenum,
        EGLClientBuffer, const EGLint *);
typedef EGLBoolean (*egl_destroy_image_khr_fn)(EGLDisplay, EGLImageKHR);

static int has_extension(const char *extensions, const char *name) {
    const size_t length = strlen(name);
    const char *cursor = extensions;
    if (!extensions || !*name || strchr(name, ' ')) return 0;
    while ((cursor = strstr(cursor, name))) {
        if ((cursor == extensions || cursor[-1] == ' ') &&
                (cursor[length] == '\0' || cursor[length] == ' ')) return 1;
        cursor += length;
    }
    return 0;
}

static int connect_socket(const char *path) {
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(address.sun_path)) { close(fd); errno = ENAMETOOLONG; return -1; }
    strcpy(address.sun_path, path);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd); return -1;
    }
    return fd;
}

static int send_buffer(int socket_fd, const struct trierarch_gpu_probe_buffer *buffer,
        int buffer_fd) {
    char control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec iov = { .iov_base = (void *)buffer, .iov_len = sizeof(*buffer) };
    struct msghdr message = { .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control, .msg_controllen = sizeof(control) };
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &buffer_fd, sizeof(buffer_fd));
    return sendmsg(socket_fd, &message, 0) == (ssize_t)sizeof(*buffer) ? 0 : -1;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s /path/to/gpu-probe.sock\n", argv[0]);
        return 64;
    }
    const char *socket_path = argv[1];
    /* The KGSL Mesa path has been verified through Wayland EGL.  Do not use
     * EGL_DEFAULT_DISPLAY here: on this device it selects an uninitializable
     * default/surfaceless path and would test nothing about buffer sharing. */
    struct wl_display *wayland_display = wl_display_connect(NULL);
    if (!wayland_display) {
        const char *display_name = getenv("WAYLAND_DISPLAY");
        fprintf(stderr, "guest: wl_display_connect(%s) failed: %s\n",
                display_name ? display_name : "wayland-0", strerror(errno));
        return 1;
    }
    EGLDisplay display = eglGetDisplay((EGLNativeDisplayType)wayland_display);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL)) {
        fprintf(stderr, "guest: eglInitialize failed: 0x%x\n", eglGetError());
        wl_display_disconnect(wayland_display);
        return 1;
    }
    const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
    printf("guest: EGL extensions: %s\n", extensions ? extensions : "(none)");
    const EGLint config_attributes[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8, EGL_NONE };
    EGLConfig config;
    EGLint count = 0;
    if (!eglChooseConfig(display, config_attributes, &config, 1, &count) || !count) {
        fprintf(stderr, "guest: no GLES pbuffer config: 0x%x\n", eglGetError());
        eglTerminate(display); wl_display_disconnect(wayland_display); return 3;
    }
    const EGLint context_attributes[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    const EGLint surface_attributes[] = { EGL_WIDTH, 256, EGL_HEIGHT, 256, EGL_NONE };
    EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attributes);
    if (context == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE ||
            !eglMakeCurrent(display, surface, surface, context)) {
        fprintf(stderr, "guest: context setup failed: 0x%x\n", eglGetError());
        eglTerminate(display); wl_display_disconnect(wayland_display); return 4;
    }
    printf("guest: GL_VENDOR=%s\n", glGetString(GL_VENDOR));
    printf("guest: GL_RENDERER=%s\n", glGetString(GL_RENDERER));
    if (!has_extension(extensions, "EGL_MESA_image_dma_buf_export")) {
        fprintf(stderr, "guest: EGL_MESA_image_dma_buf_export unavailable\n");
        eglDestroySurface(display, surface);
        eglDestroyContext(display, context);
        eglTerminate(display);
        wl_display_disconnect(wayland_display);
        return 2;
    }

    GLuint texture = 0, framebuffer = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 256, 0, GL_RGBA,
            GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "guest: framebuffer unavailable\n");
        return 5;
    }
    glViewport(0, 0, 256, 256);
    glClearColor(0.1f, 0.75f, 0.95f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();

    egl_create_image_khr_fn create_image = (egl_create_image_khr_fn)eglGetProcAddress("eglCreateImageKHR");
    egl_destroy_image_khr_fn destroy_image = (egl_destroy_image_khr_fn)eglGetProcAddress("eglDestroyImageKHR");
    egl_export_dmabuf_query_mesa_fn query_export = (egl_export_dmabuf_query_mesa_fn)
            eglGetProcAddress("eglExportDMABUFImageQueryMESA");
    egl_export_dmabuf_mesa_fn export_image = (egl_export_dmabuf_mesa_fn)
            eglGetProcAddress("eglExportDMABUFImageMESA");
    if (!create_image || !destroy_image || !query_export || !export_image) {
        fprintf(stderr, "guest: dma-buf export entry points unavailable\n");
        return 6;
    }
    EGLImageKHR image = create_image(display, context, EGL_GL_TEXTURE_2D_KHR,
            (EGLClientBuffer)(uintptr_t)texture, NULL);
    if (image == EGL_NO_IMAGE_KHR) {
        fprintf(stderr, "guest: eglCreateImageKHR failed: 0x%x\n", eglGetError());
        return 7;
    }
    int fourcc = 0, planes = 0;
    EGLuint64KHR modifier = 0;
    if (!query_export(display, image, &fourcc, &planes, &modifier) || planes != 1) {
        fprintf(stderr, "guest: dma-buf export query failed/unsupported planes=%d: 0x%x\n",
                planes, eglGetError());
        destroy_image(display, image); return 8;
    }
    int buffer_fd = -1;
    EGLint stride = 0, offset = 0;
    if (!export_image(display, image, &buffer_fd, &stride, &offset) || buffer_fd < 0 || offset != 0) {
        fprintf(stderr, "guest: dma-buf export failed: fd=%d offset=%d egl=0x%x\n",
                buffer_fd, offset, eglGetError());
        destroy_image(display, image); return 9;
    }
    struct trierarch_gpu_probe_buffer buffer = {
        .magic = TRIERARCH_GPU_PROBE_MAGIC, .version = TRIERARCH_GPU_PROBE_VERSION,
        .type = TRIERARCH_GPU_PROBE_BUFFER, .width = 256, .height = 256,
        .drm_format = (uint32_t)fourcc, .stride = (uint32_t)stride, .modifier = modifier,
    };
    printf("guest: export format=0x%x stride=%d modifier=0x%llx\n", fourcc, stride,
            (unsigned long long)modifier);
    int socket_fd = connect_socket(socket_path);
    if (socket_fd < 0 || send_buffer(socket_fd, &buffer, buffer_fd) < 0) {
        fprintf(stderr, "guest: send buffer failed: %s\n", strerror(errno));
        if (socket_fd >= 0) close(socket_fd);
        close(buffer_fd); destroy_image(display, image); return 10;
    }
    struct trierarch_gpu_probe_result_message result = {0};
    ssize_t received = recv(socket_fd, &result, sizeof(result), 0);
    if (received == sizeof(result) && result.magic == TRIERARCH_GPU_PROBE_MAGIC) {
        printf("guest: host result=%u egl_error=0x%x\n", result.result, result.egl_error);
    } else {
        fprintf(stderr, "guest: host result unavailable\n");
    }
    close(socket_fd); close(buffer_fd); destroy_image(display, image);
    glDeleteFramebuffers(1, &framebuffer); glDeleteTextures(1, &texture);
    eglDestroySurface(display, surface); eglDestroyContext(display, context); eglTerminate(display);
    wl_display_disconnect(wayland_display);
    return result.result == TRIERARCH_GPU_PROBE_OK ? 0 : 11;
}
