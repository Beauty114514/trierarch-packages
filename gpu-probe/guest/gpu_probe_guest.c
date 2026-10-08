#define _POSIX_C_SOURCE 200809L
#include "../protocol.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <wayland-client.h>
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define GPU_PROBE_BUFFER_COUNT 3u
#define GPU_PROBE_FRAME_COUNT 180u

#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif
#ifndef EGL_PLATFORM_WAYLAND_KHR
#define EGL_PLATFORM_WAYLAND_KHR 0x31D8
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

struct guest_slot {
    struct trierarch_gpu_probe_buffer buffer;
    EGLImageKHR image;
    GLuint texture;
    GLuint framebuffer;
    int reuse_fence_fd;
    bool received;
    bool in_flight;
};

struct received_host_buffer {
    struct trierarch_gpu_probe_buffer buffer;
    int fds[TRIERARCH_GPU_PROBE_MAX_NATIVE_HANDLE_FDS];
    uint8_t serialized_handle[TRIERARCH_GPU_PROBE_MAX_SERIALIZED_HANDLE_BYTES];
    size_t serialized_size;
    uint32_t fd_count;
};

static uint64_t monotonic_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static int has_extension(const char *extensions, const char *name) {
    if (!extensions || !*name || strchr(name, ' ')) return 0;
    size_t length = strlen(name); const char *cursor = extensions;
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

static int send_hello(int fd) {
    const struct trierarch_gpu_probe_hello hello = {
        .magic = TRIERARCH_GPU_PROBE_MAGIC, .version = TRIERARCH_GPU_PROBE_VERSION,
        .type = TRIERARCH_GPU_PROBE_HELLO, .direction = TRIERARCH_GPU_PROBE_HOST_TO_GUEST,
    };
    return send(fd, &hello, sizeof(hello), 0) == (ssize_t)sizeof(hello) ? 0 : -1;
}

static int send_result(int fd, uint32_t result, uint32_t egl_error,
        uint32_t buffer_id, int fence_fd) {
    const struct trierarch_gpu_probe_result_message message = {
        .magic = TRIERARCH_GPU_PROBE_MAGIC, .version = TRIERARCH_GPU_PROBE_VERSION,
        .type = TRIERARCH_GPU_PROBE_RESULT, .result = result, .egl_error = egl_error,
        .buffer_id = buffer_id,
    };
    char control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec iov = { .iov_base = (void *)&message, .iov_len = sizeof(message) };
    struct msghdr packet = { .msg_iov = &iov, .msg_iovlen = 1 };
    if (fence_fd >= 0) {
        packet.msg_control = control; packet.msg_controllen = sizeof(control);
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&packet);
        cmsg->cmsg_level = SOL_SOCKET; cmsg->cmsg_type = SCM_RIGHTS; cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &fence_fd, sizeof(fence_fd));
    }
    return sendmsg(fd, &packet, 0) == (ssize_t)sizeof(message) ? 0 : -1;
}

static void close_received_host_buffer(struct received_host_buffer *received) {
    if (!received) return;
    for (uint32_t index = 0; index < received->fd_count; ++index) {
        if (received->fds[index] >= 0) close(received->fds[index]);
        received->fds[index] = -1;
    }
}

static int receive_buffer(int fd, struct received_host_buffer *received) {
    memset(received, 0, sizeof(*received));
    for (uint32_t index = 0; index < TRIERARCH_GPU_PROBE_MAX_NATIVE_HANDLE_FDS; ++index)
        received->fds[index] = -1;
    if (recv(fd, &received->buffer, sizeof(received->buffer), 0) != (ssize_t)sizeof(received->buffer))
        return -1;
    struct trierarch_gpu_probe_buffer *buffer = &received->buffer;
    if (buffer->magic != TRIERARCH_GPU_PROBE_MAGIC || buffer->version != TRIERARCH_GPU_PROBE_VERSION ||
            buffer->type != TRIERARCH_GPU_PROBE_HOST_BUFFER || buffer->buffer_id >= GPU_PROBE_BUFFER_COUNT)
        return -1;

    char control[CMSG_SPACE(sizeof(int) * TRIERARCH_GPU_PROBE_MAX_NATIVE_HANDLE_FDS)] = {0};
    struct iovec iov = {
        .iov_base = received->serialized_handle,
        .iov_len = sizeof(received->serialized_handle),
    };
    struct msghdr packet = { .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control, .msg_controllen = sizeof(control) };
    ssize_t byte_count = recvmsg(fd, &packet, 0);
    if (byte_count <= 0 || (packet.msg_flags & (MSG_CTRUNC | MSG_TRUNC)))
        return -1;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&packet);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
            cmsg->cmsg_len < CMSG_LEN(0)) return -1;
    size_t fd_bytes = cmsg->cmsg_len - CMSG_LEN(0);
    if (fd_bytes % sizeof(int) || fd_bytes / sizeof(int) < 1 ||
            fd_bytes / sizeof(int) > TRIERARCH_GPU_PROBE_MAX_NATIVE_HANDLE_FDS)
        return -1;
    received->serialized_size = (size_t)byte_count;
    received->fd_count = (uint32_t)(fd_bytes / sizeof(int));
    memcpy(received->fds, CMSG_DATA(cmsg), fd_bytes);
    return 0;
}

static int wait_fence(int fence_fd, uint64_t *wait_ns) {
    struct pollfd descriptor = { .fd = fence_fd, .events = POLLIN };
    uint64_t started = monotonic_ns(); int result;
    do { result = poll(&descriptor, 1, 3000); } while (result < 0 && errno == EINTR);
    *wait_ns += monotonic_ns() - started;
    close(fence_fd);
    return result == 1 && !(descriptor.revents & (POLLERR | POLLNVAL));
}

static int receive_host_result(int fd, struct guest_slot slots[GPU_PROBE_BUFFER_COUNT],
        uint32_t *acknowledged) {
    struct trierarch_gpu_probe_result_message result = {0};
    char control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec iov = { .iov_base = &result, .iov_len = sizeof(result) };
    struct msghdr packet = { .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control, .msg_controllen = sizeof(control) };
    if (recvmsg(fd, &packet, 0) != (ssize_t)sizeof(result) || result.magic != TRIERARCH_GPU_PROBE_MAGIC ||
            result.version != TRIERARCH_GPU_PROBE_VERSION || result.type != TRIERARCH_GPU_PROBE_RESULT ||
            result.buffer_id >= GPU_PROBE_BUFFER_COUNT) return -1;
    struct guest_slot *slot = &slots[result.buffer_id];
    if (result.result != TRIERARCH_GPU_PROBE_OK || !slot->in_flight) {
        fprintf(stderr, "guest: host rejected slot=%u result=%u egl=0x%x\n",
                result.buffer_id, result.result, result.egl_error); return -1;
    }
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&packet);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
            cmsg->cmsg_len != CMSG_LEN(sizeof(int))) return -1;
    memcpy(&slot->reuse_fence_fd, CMSG_DATA(cmsg), sizeof(slot->reuse_fence_fd));
    if (slot->reuse_fence_fd < 0) return -1;
    slot->in_flight = false; ++*acknowledged;
    return 0;
}

static struct wl_display *probe_wayland_display;

static EGLDisplay create_probe_display(const char **path) {
    const char *client_extensions = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    fprintf(stderr, "guest: EGL client platforms base=%d wayland=%d surfaceless=%d\n",
            has_extension(client_extensions, "EGL_EXT_platform_base") ||
                    has_extension(client_extensions, "EGL_KHR_platform_base"),
            has_extension(client_extensions, "EGL_EXT_platform_wayland") ||
                    has_extension(client_extensions, "EGL_KHR_platform_wayland"),
            has_extension(client_extensions, "EGL_MESA_platform_surfaceless"));
    get_platform_display_fn function = (get_platform_display_fn)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!function) function = (get_platform_display_fn)eglGetProcAddress("eglGetPlatformDisplay");
    if (function) {
        EGLDisplay display = function(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
        if (display != EGL_NO_DISPLAY) {
            *path = "explicit-surfaceless";
            return display;
        }
        fprintf(stderr, "guest: explicit surfaceless display rejected: 0x%x\n", eglGetError());
    }

    EGLDisplay display = EGL_NO_DISPLAY;
    if (getenv("EGL_PLATFORM")) {
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display != EGL_NO_DISPLAY) {
            *path = "environment-default";
            return display;
        }
    }

    probe_wayland_display = wl_display_connect(NULL);
    if (!probe_wayland_display) {
        fprintf(stderr, "guest: Wayland EGL fallback could not connect: %s\n", strerror(errno));
        return EGL_NO_DISPLAY;
    }
    /* eglGetPlatformDisplay* is absent or rejects both standard platform
     * enums on this Mesa build. The legacy Wayland native-display entry point
     * is still part of EGL 1.4 and is widely supported by Mesa. */
    if (setenv("EGL_PLATFORM", "wayland", 1) != 0)
        fprintf(stderr, "guest: could not select Wayland EGL platform: %s\n", strerror(errno));
    display = eglGetDisplay((EGLNativeDisplayType)probe_wayland_display);
    if (display == EGL_NO_DISPLAY) {
        fprintf(stderr, "guest: legacy Wayland EGL display rejected: 0x%x\n", eglGetError());
        wl_display_disconnect(probe_wayland_display);
        probe_wayland_display = NULL;
        return EGL_NO_DISPLAY;
    }
    *path = "parent-wayland";
    return display;
}

static int run_receive_only_probe(const char *socket_path) {
    int socket_fd = connect_socket(socket_path);
    if (socket_fd < 0 || send_hello(socket_fd) < 0) {
        fprintf(stderr, "guest: connect/hello failed: %s\n", strerror(errno));
        return 1;
    }

    for (uint32_t received = 0; received < GPU_PROBE_BUFFER_COUNT; ++received) {
        struct received_host_buffer host_buffer;
        if (receive_buffer(socket_fd, &host_buffer) < 0) {
            fprintf(stderr, "guest: failed to receive host buffer %u\n", received);
            close(socket_fd);
            return 2;
        }

        const struct trierarch_gpu_probe_buffer *buffer = &host_buffer.buffer;
        printf("guest: received slot=%u %ux%u format=0x%x stride=%u modifier=0x%llx official-handle bytes=%zu fds=%u\n",
                buffer->buffer_id, buffer->width, buffer->height, buffer->drm_format, buffer->stride,
                (unsigned long long)buffer->modifier, host_buffer.serialized_size, host_buffer.fd_count);
        for (uint32_t index = 0; index < host_buffer.fd_count; ++index) {
            struct stat status = {0};
            if (fstat(host_buffer.fds[index], &status) < 0) {
                fprintf(stderr, "guest: fstat host buffer %u fd %u failed: %s\n", received, index, strerror(errno));
                close_received_host_buffer(&host_buffer);
                close(socket_fd);
                return 3;
            }
            printf("guest: slot=%u handle-fd[%u]=%d mode=%#o\n", buffer->buffer_id,
                    index, host_buffer.fds[index], status.st_mode);
        }
        close_received_host_buffer(&host_buffer);
    }

    puts("guest: host-to-guest AHardwareBuffer transport verified");
    close(socket_fd);
    return 0;
}

static int run_probe(const char *socket_path) {
    int socket_fd = connect_socket(socket_path);
    if (socket_fd < 0 || send_hello(socket_fd) < 0) { fprintf(stderr, "guest: connect/hello failed: %s\n", strerror(errno)); return 1; }
    const char *display_path = "none";
    EGLDisplay display = create_probe_display(&display_path);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL)) {
        fprintf(stderr, "guest: %s EGL initialization failed: 0x%x\n", display_path, eglGetError());
        return 2;
    }
    printf("guest: EGL display path=%s\n", display_path);
    const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
    printf("guest: EGL import=%d native-fence=%d\n", has_extension(extensions, "EGL_EXT_image_dma_buf_import"),
            has_extension(extensions, "EGL_ANDROID_native_fence_sync"));
    const EGLint config_attributes[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
    EGLConfig config; EGLint count = 0;
    const EGLint context_attributes[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    if (!eglChooseConfig(display, config_attributes, &config, 1, &count) || !count) return 3;
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    const EGLint surface_attributes[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attributes);
    if (context == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE || !eglMakeCurrent(display, surface, surface, context)) return 4;
    printf("guest: GL_VENDOR=%s GL_RENDERER=%s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER));
    create_image_fn create_image = (create_image_fn)eglGetProcAddress("eglCreateImageKHR");
    destroy_image_fn destroy_image = (destroy_image_fn)eglGetProcAddress("eglDestroyImageKHR");
    image_target_fn image_target = (image_target_fn)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    create_sync_fn create_sync = (create_sync_fn)eglGetProcAddress("eglCreateSyncKHR");
    destroy_sync_fn destroy_sync = (destroy_sync_fn)eglGetProcAddress("eglDestroySyncKHR");
    dup_native_fence_fd_fn dup_fence = (dup_native_fence_fd_fn)eglGetProcAddress("eglDupNativeFenceFDANDROID");
    bool native_fence_supported = has_extension(extensions, "EGL_ANDROID_native_fence_sync") &&
            create_sync && destroy_sync && dup_fence;
    if (!create_image || !destroy_image || !image_target) return 5;
    struct guest_slot slots[GPU_PROBE_BUFFER_COUNT] = {0};
    for (uint32_t received = 0; received < GPU_PROBE_BUFFER_COUNT; ++received) {
        struct received_host_buffer host_buffer;
        if (receive_buffer(socket_fd, &host_buffer) < 0 ||
                slots[host_buffer.buffer.buffer_id].received) return 6;
        struct trierarch_gpu_probe_buffer buffer = host_buffer.buffer;
        int buffer_fd = host_buffer.fds[0];
        struct guest_slot *slot = &slots[buffer.buffer_id]; slot->buffer = buffer; slot->reuse_fence_fd = -1;
        const EGLint attributes[] = { EGL_WIDTH, (EGLint)buffer.width, EGL_HEIGHT, (EGLint)buffer.height,
            EGL_LINUX_DRM_FOURCC_EXT, (EGLint)buffer.drm_format, EGL_DMA_BUF_PLANE0_FD_EXT, buffer_fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0, EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)buffer.stride, EGL_NONE };
        slot->image = create_image(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attributes);
        close_received_host_buffer(&host_buffer);
        if (slot->image == EGL_NO_IMAGE_KHR) return 7;
        glGenTextures(1, &slot->texture); glBindTexture(GL_TEXTURE_2D, slot->texture); image_target(GL_TEXTURE_2D, slot->image);
        glGenFramebuffers(1, &slot->framebuffer); glBindFramebuffer(GL_FRAMEBUFFER, slot->framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, slot->texture, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return 8;
        slot->received = true;
        printf("guest: imported slot=%u %ux%u\n", buffer.buffer_id, buffer.width, buffer.height);
    }
    uint64_t started = monotonic_ns(), wait_ns = 0; uint32_t submitted = 0, acknowledged = 0;
    for (uint32_t frame = 0; frame < GPU_PROBE_FRAME_COUNT; ++frame) {
        uint32_t index = frame % GPU_PROBE_BUFFER_COUNT; struct guest_slot *slot = &slots[index];
        while (slot->in_flight) if (receive_host_result(socket_fd, slots, &acknowledged) < 0) return 9;
        if (slot->reuse_fence_fd >= 0) {
            int fence_fd = slot->reuse_fence_fd; slot->reuse_fence_fd = -1;
            if (!wait_fence(fence_fd, &wait_ns)) return 10;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, slot->framebuffer);
        float phase = (float)(frame % 60) / 59.0f;
        glViewport(0, 0, (GLsizei)slot->buffer.width, (GLsizei)slot->buffer.height);
        glClearColor(phase, 0.85f - phase * 0.5f, 0.18f + phase * 0.6f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
        int fence_fd = -1;
        if (native_fence_supported) {
            EGLSyncKHR sync = create_sync(display, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
            if (sync == EGL_NO_SYNC_KHR) return 11;
            glFlush(); fence_fd = dup_fence(display, sync); destroy_sync(display, sync);
        } else {
            /* Probe-only: do not turn this into a production queue policy. */
            glFinish();
        }
        if ((native_fence_supported && fence_fd < 0) || glGetError() != GL_NO_ERROR ||
                send_result(socket_fd, TRIERARCH_GPU_PROBE_OK, EGL_SUCCESS, index, fence_fd) < 0) {
            if (fence_fd >= 0) close(fence_fd);
            return 12;
        }
        close(fence_fd); slot->in_flight = true; ++submitted;
    }
    while (acknowledged < submitted) if (receive_host_result(socket_fd, slots, &acknowledged) < 0) return 13;
    for (uint32_t index = 0; index < GPU_PROBE_BUFFER_COUNT; ++index) {
        if (slots[index].reuse_fence_fd >= 0 && !wait_fence(slots[index].reuse_fence_fd, &wait_ns)) return 14;
        slots[index].reuse_fence_fd = -1;
    }
    uint64_t elapsed_ns = monotonic_ns() - started;
    printf("guest: pool frames=%u acknowledged=%u elapsed=%.2fms fps=%.1f reuse-wait=%.2fms\n",
            submitted, acknowledged, elapsed_ns / 1e6, (double)submitted * 1e9 / elapsed_ns, wait_ns / 1e6);
    for (uint32_t index = 0; index < GPU_PROBE_BUFFER_COUNT; ++index) {
        glDeleteFramebuffers(1, &slots[index].framebuffer); glDeleteTextures(1, &slots[index].texture);
        destroy_image(display, slots[index].image);
    }
    eglDestroySurface(display, surface); eglDestroyContext(display, context); eglTerminate(display);
    if (probe_wayland_display) { wl_display_disconnect(probe_wayland_display); probe_wayland_display = NULL; }
    close(socket_fd);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--receive-only") == 0)
        return run_receive_only_probe(argv[2]);
    if (argc == 2)
        return run_probe(argv[1]);
    fprintf(stderr, "usage: %s [--receive-only] /path/to/gpu-probe.sock\n", argv[0]);
    return 64;
}
