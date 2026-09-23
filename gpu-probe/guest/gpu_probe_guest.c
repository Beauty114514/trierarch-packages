#define _POSIX_C_SOURCE 200809L
#include "../protocol.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
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

typedef EGLDisplay (*get_platform_display_fn)(EGLenum, void *, const EGLint *);
typedef EGLImageKHR (*create_image_fn)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
typedef EGLBoolean (*destroy_image_fn)(EGLDisplay, EGLImageKHR);
typedef void (*image_target_fn)(GLenum, GLeglImageOES);
typedef EGLSyncKHR (*create_sync_fn)(EGLDisplay, EGLenum, const EGLint *);
typedef EGLBoolean (*destroy_sync_fn)(EGLDisplay, EGLSyncKHR);
typedef EGLint (*dup_native_fence_fd_fn)(EGLDisplay, EGLSyncKHR);

static int has_extension(const char *extensions, const char *name) {
    if (!extensions || !*name || strchr(name, ' ')) return 0;
    size_t length = strlen(name);
    const char *cursor = extensions;
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
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) { close(fd); return -1; }
    return fd;
}

static int send_hello(int fd, uint32_t direction) {
    const struct trierarch_gpu_probe_hello hello = {
        .magic = TRIERARCH_GPU_PROBE_MAGIC, .version = TRIERARCH_GPU_PROBE_VERSION,
        .type = TRIERARCH_GPU_PROBE_HELLO, .direction = direction,
    };
    return send(fd, &hello, sizeof(hello), 0) == (ssize_t)sizeof(hello) ? 0 : -1;
}

static int send_result(int fd, uint32_t result, uint32_t egl_error, int fence_fd) {
    const struct trierarch_gpu_probe_result_message message = {
        .magic = TRIERARCH_GPU_PROBE_MAGIC, .version = TRIERARCH_GPU_PROBE_VERSION,
        .type = TRIERARCH_GPU_PROBE_RESULT, .result = result, .egl_error = egl_error,
    };
    if (fence_fd < 0)
        return send(fd, &message, sizeof(message), 0) == (ssize_t)sizeof(message) ? 0 : -1;
    char control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec iov = { .iov_base = (void *)&message, .iov_len = sizeof(message) };
    struct msghdr packet = { .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control, .msg_controllen = sizeof(control) };
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&packet);
    cmsg->cmsg_level = SOL_SOCKET; cmsg->cmsg_type = SCM_RIGHTS; cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fence_fd, sizeof(fence_fd));
    return sendmsg(fd, &packet, 0) == (ssize_t)sizeof(message) ? 0 : -1;
}

static int receive_buffer(int fd, struct trierarch_gpu_probe_buffer *buffer, int *buffer_fd) {
    char control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec iov = { .iov_base = buffer, .iov_len = sizeof(*buffer) };
    struct msghdr message = { .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control, .msg_controllen = sizeof(control) };
    if (recvmsg(fd, &message, 0) != (ssize_t)sizeof(*buffer)) return -1;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
            cmsg->cmsg_len != CMSG_LEN(sizeof(int))) return -1;
    memcpy(buffer_fd, CMSG_DATA(cmsg), sizeof(*buffer_fd));
    return buffer->magic == TRIERARCH_GPU_PROBE_MAGIC &&
            buffer->version == TRIERARCH_GPU_PROBE_VERSION &&
            buffer->type == TRIERARCH_GPU_PROBE_HOST_BUFFER && *buffer_fd >= 0 ? 0 : -1;
}

static int wait_native_fence(int fence_fd) {
    if (fence_fd < 0) return 0;
    struct pollfd descriptor = { .fd = fence_fd, .events = POLLIN };
    int result;
    do { result = poll(&descriptor, 1, 3000); } while (result < 0 && errno == EINTR);
    close(fence_fd);
    return result == 1 && !(descriptor.revents & (POLLERR | POLLNVAL));
}

static int receive_host_result(int fd) {
    struct trierarch_gpu_probe_result_message result = {0};
    char control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec iov = { .iov_base = &result, .iov_len = sizeof(result) };
    struct msghdr packet = { .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control, .msg_controllen = sizeof(control) };
    if (recvmsg(fd, &packet, 0) != (ssize_t)sizeof(result) ||
            result.magic != TRIERARCH_GPU_PROBE_MAGIC || result.type != TRIERARCH_GPU_PROBE_RESULT) {
        fprintf(stderr, "guest: host completion unavailable\n"); return 1;
    }
    printf("guest: host sample result=%u egl_error=0x%x\n", result.result, result.egl_error);
    if (result.result != TRIERARCH_GPU_PROBE_OK) return 1;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&packet);
    int fence_fd = -1;
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
            cmsg->cmsg_len != CMSG_LEN(sizeof(int))) {
        fprintf(stderr, "guest: host completion lacks reuse fence\n"); return 1;
    }
    memcpy(&fence_fd, CMSG_DATA(cmsg), sizeof(fence_fd));
    if (!wait_native_fence(fence_fd)) {
        fprintf(stderr, "guest: host reuse fence wait failed: %s\n", strerror(errno)); return 1;
    }
    printf("guest: host reuse fence signaled\n");
    return 0;
}

static EGLDisplay create_surfaceless_display(void) {
    get_platform_display_fn get_platform_display = (get_platform_display_fn)
            eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!get_platform_display) get_platform_display = (get_platform_display_fn)
            eglGetProcAddress("eglGetPlatformDisplay");
    return get_platform_display ? get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL)
                                : EGL_NO_DISPLAY;
}

static int run_host_to_guest(const char *socket_path) {
    int socket_fd = connect_socket(socket_path);
    if (socket_fd < 0 || send_hello(socket_fd, TRIERARCH_GPU_PROBE_HOST_TO_GUEST) < 0) {
        fprintf(stderr, "guest: connect/hello failed: %s\n", strerror(errno)); if (socket_fd >= 0) close(socket_fd); return 1;
    }
    struct trierarch_gpu_probe_buffer buffer = {0}; int buffer_fd = -1;
    if (receive_buffer(socket_fd, &buffer, &buffer_fd) < 0) {
        fprintf(stderr, "guest: Android buffer receive failed\n"); close(socket_fd); return 2;
    }
    printf("guest: Android buffer %ux%u format=0x%x stride=%u modifier=0x%llx\n",
            buffer.width, buffer.height, buffer.drm_format, buffer.stride, (unsigned long long)buffer.modifier);

    EGLDisplay display = create_surfaceless_display();
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL)) {
        EGLint error = eglGetError(); fprintf(stderr, "guest: surfaceless eglInitialize failed: 0x%x\n", error);
        send_result(socket_fd, TRIERARCH_GPU_PROBE_IMPORT_UNAVAILABLE, error, -1); close(buffer_fd); close(socket_fd); return 3;
    }
    const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
    printf("guest: surfaceless EGL extensions: %s\n", extensions ? extensions : "(none)");
    if (!has_extension(extensions, "EGL_EXT_image_dma_buf_import")) {
        fprintf(stderr, "guest: EGL_EXT_image_dma_buf_import unavailable\n");
        send_result(socket_fd, TRIERARCH_GPU_PROBE_IMPORT_UNAVAILABLE, EGL_SUCCESS, -1);
        close(buffer_fd); eglTerminate(display); close(socket_fd); return 4;
    }
    const EGLint config_attributes[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
    EGLConfig config; EGLint count = 0;
    const EGLint context_attributes[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    if (!eglChooseConfig(display, config_attributes, &config, 1, &count) || !count) {
        EGLint error = eglGetError(); fprintf(stderr, "guest: no surfaceless GLES config: 0x%x\n", error);
        send_result(socket_fd, TRIERARCH_GPU_PROBE_IMPORT_UNAVAILABLE, error, -1); close(buffer_fd); eglTerminate(display); close(socket_fd); return 5;
    }
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    const EGLint surface_attributes[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attributes);
    if (context == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE || !eglMakeCurrent(display, surface, surface, context)) {
        EGLint error = eglGetError(); fprintf(stderr, "guest: surfaceless context setup failed: 0x%x\n", error);
        send_result(socket_fd, TRIERARCH_GPU_PROBE_IMPORT_UNAVAILABLE, error, -1); close(buffer_fd); eglTerminate(display); close(socket_fd); return 6;
    }
    printf("guest: GL_VENDOR=%s\nguest: GL_RENDERER=%s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER));
    create_image_fn create_image = (create_image_fn)eglGetProcAddress("eglCreateImageKHR");
    destroy_image_fn destroy_image = (destroy_image_fn)eglGetProcAddress("eglDestroyImageKHR");
    image_target_fn image_target = (image_target_fn)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    create_sync_fn create_sync = (create_sync_fn)eglGetProcAddress("eglCreateSyncKHR");
    destroy_sync_fn destroy_sync = (destroy_sync_fn)eglGetProcAddress("eglDestroySyncKHR");
    dup_native_fence_fd_fn dup_native_fence_fd = (dup_native_fence_fd_fn)
            eglGetProcAddress("eglDupNativeFenceFDANDROID");
    const EGLint attributes[] = { EGL_WIDTH, (EGLint)buffer.width, EGL_HEIGHT, (EGLint)buffer.height,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)buffer.drm_format, EGL_DMA_BUF_PLANE0_FD_EXT, buffer_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0, EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)buffer.stride, EGL_NONE };
    EGLImageKHR image = create_image ? create_image(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attributes) : EGL_NO_IMAGE_KHR;
    close(buffer_fd);
    if (!image || !destroy_image || !image_target) {
        EGLint error = eglGetError(); fprintf(stderr, "guest: Android dma-buf import failed: 0x%x\n", error);
        send_result(socket_fd, TRIERARCH_GPU_PROBE_IMPORT_FAILED, error, -1);
        eglDestroySurface(display, surface); eglDestroyContext(display, context); eglTerminate(display); close(socket_fd); return 7;
    }
    GLuint texture = 0, framebuffer = 0; glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    image_target(GL_TEXTURE_2D, image);
    glGenFramebuffers(1, &framebuffer); glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "guest: imported framebuffer incomplete: 0x%x\n", status);
        send_result(socket_fd, TRIERARCH_GPU_PROBE_DRAW_FAILED, EGL_SUCCESS, -1);
    } else {
        glViewport(0, 0, (GLsizei)buffer.width, (GLsizei)buffer.height);
        glClearColor(0.12f, 0.78f, 0.95f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
        GLenum error = glGetError();
        int fence_fd = -1;
        if (error == GL_NO_ERROR && create_sync && destroy_sync && dup_native_fence_fd) {
            EGLSyncKHR sync = create_sync(display, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
            if (sync != EGL_NO_SYNC_KHR) {
                /* Submit the clear; this sync_file signals once the guest write is visible. */
                glFlush();
                fence_fd = dup_native_fence_fd(display, sync);
                destroy_sync(display, sync);
            }
        }
        printf("guest: Android buffer rendered, gl_error=0x%x release_fence=%d\n", error, fence_fd);
        if (error == GL_NO_ERROR && fence_fd >= 0) {
            send_result(socket_fd, TRIERARCH_GPU_PROBE_OK, EGL_SUCCESS, fence_fd);
            close(fence_fd);
        } else {
            send_result(socket_fd, TRIERARCH_GPU_PROBE_DRAW_FAILED, eglGetError(), -1);
        }
    }
    glDeleteFramebuffers(1, &framebuffer); glDeleteTextures(1, &texture); destroy_image(display, image);
    eglDestroySurface(display, surface); eglDestroyContext(display, context); eglTerminate(display);
    int result = receive_host_result(socket_fd); close(socket_fd); return result;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s /path/to/gpu-probe.sock\n", argv[0]); return 64;
    }
    return run_host_to_guest(argv[1]);
}
