#define _POSIX_C_SOURCE 200809L
#include "gpu_probe_host.h"
#include "../protocol.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <android/log.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define LOG_TAG "trierarch-gpu-probe"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#ifndef EGL_LINUX_DMA_BUF_EXT
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
#define EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT 0x3443
#define EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT 0x3444
#endif

typedef EGLImageKHR (*create_image_fn)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer,
        const EGLint *);
typedef EGLBoolean (*destroy_image_fn)(EGLDisplay, EGLImageKHR);
typedef void (*image_target_fn)(GLenum, GLeglImageOES);

static int has_extension(const char *extensions, const char *name) {
    size_t length = strlen(name); const char *cursor = extensions;
    if (!extensions || strchr(name, ' ')) return 0;
    while ((cursor = strstr(cursor, name))) {
        if ((cursor == extensions || cursor[-1] == ' ') &&
                (cursor[length] == '\0' || cursor[length] == ' ')) return 1;
        cursor += length;
    }
    return 0;
}

static int receive_buffer(int client_fd, struct trierarch_gpu_probe_buffer *buffer) {
    char control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec iov = { .iov_base = buffer, .iov_len = sizeof(*buffer) };
    struct msghdr message = { .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control, .msg_controllen = sizeof(control) };
    if (recvmsg(client_fd, &message, 0) != (ssize_t)sizeof(*buffer)) return -1;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
            cmsg->cmsg_len != CMSG_LEN(sizeof(int))) return -1;
    int fd = -1; memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
    return fd;
}

static void send_result(int client_fd, uint32_t result, EGLint egl_error) {
    const struct trierarch_gpu_probe_result_message message = {
        .magic = TRIERARCH_GPU_PROBE_MAGIC, .version = TRIERARCH_GPU_PROBE_VERSION,
        .type = TRIERARCH_GPU_PROBE_RESULT, .result = result, .egl_error = (uint32_t)egl_error,
    };
    (void)send(client_fd, &message, sizeof(message), MSG_NOSIGNAL);
}

static GLuint make_program(void) {
    const char *vertex = "attribute vec2 p; varying vec2 uv; void main(){uv=(p+1.0)*.5;gl_Position=vec4(p,0,1);}";
    const char *fragment = "precision mediump float; varying vec2 uv; uniform sampler2D t; void main(){gl_FragColor=texture2D(t,uv);}";
    GLuint shaders[2] = { glCreateShader(GL_VERTEX_SHADER), glCreateShader(GL_FRAGMENT_SHADER) };
    glShaderSource(shaders[0], 1, &vertex, NULL); glCompileShader(shaders[0]);
    glShaderSource(shaders[1], 1, &fragment, NULL); glCompileShader(shaders[1]);
    GLuint program = glCreateProgram();
    glAttachShader(program, shaders[0]); glAttachShader(program, shaders[1]); glLinkProgram(program);
    GLint linked = GL_FALSE; glGetProgramiv(program, GL_LINK_STATUS, &linked);
    glDeleteShader(shaders[0]); glDeleteShader(shaders[1]);
    if (!linked) { glDeleteProgram(program); return 0; }
    return program;
}

int trierarch_gpu_probe_accept_one(ANativeWindow *window, const char *socket_path) {
    if (!window || !socket_path) return -1;
    int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (listener < 0) return -2;
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (strlen(socket_path) >= sizeof(address.sun_path)) { close(listener); return -3; }
    strcpy(address.sun_path, socket_path); unlink(socket_path);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(listener, 1) < 0) {
        LOGE("socket setup failed: %s", strerror(errno)); close(listener); return -4;
    }
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLConfig config; EGLint count = 0;
    const EGLint attrs[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE,
        EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL) ||
            !eglChooseConfig(display, attrs, &config, 1, &count) || !count) goto fail;
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT,
            (EGLint[]){ EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE });
    EGLSurface surface = eglCreateWindowSurface(display, config, window, NULL);
    if (context == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE ||
            !eglMakeCurrent(display, surface, surface, context)) goto fail_egl;
    const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
    LOGI("host EGL extensions: %s", extensions ? extensions : "(none)");
    create_image_fn create_image = (create_image_fn)eglGetProcAddress("eglCreateImageKHR");
    destroy_image_fn destroy_image = (destroy_image_fn)eglGetProcAddress("eglDestroyImageKHR");
    image_target_fn image_target = (image_target_fn)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    int client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
    if (client < 0) goto fail_current;
    struct trierarch_gpu_probe_buffer buffer = {0};
    int buffer_fd = receive_buffer(client, &buffer);
    if (buffer_fd < 0 || buffer.magic != TRIERARCH_GPU_PROBE_MAGIC ||
            buffer.version != TRIERARCH_GPU_PROBE_VERSION || buffer.type != TRIERARCH_GPU_PROBE_BUFFER ||
            !buffer.width || !buffer.height || !buffer.stride) {
        send_result(client, TRIERARCH_GPU_PROBE_BAD_MESSAGE, EGL_SUCCESS); goto done_client;
    }
    if (!has_extension(extensions, "EGL_EXT_image_dma_buf_import") || !create_image ||
            !destroy_image || !image_target) {
        send_result(client, TRIERARCH_GPU_PROBE_IMPORT_UNAVAILABLE, EGL_SUCCESS); goto done_client;
    }
    const EGLint image_attrs[] = { EGL_WIDTH, (EGLint)buffer.width, EGL_HEIGHT, (EGLint)buffer.height,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)buffer.drm_format, EGL_DMA_BUF_PLANE0_FD_EXT, buffer_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0, EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)buffer.stride,
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(buffer.modifier & 0xffffffffu),
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(buffer.modifier >> 32), EGL_NONE };
    EGLImageKHR image = create_image(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, image_attrs);
    if (image == EGL_NO_IMAGE_KHR) {
        EGLint error = eglGetError(); LOGE("guest dma-buf import failed: 0x%x", error);
        send_result(client, TRIERARCH_GPU_PROBE_IMPORT_FAILED, error); goto done_client;
    }
    GLuint texture = 0; glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    image_target(GL_TEXTURE_2D, (GLeglImageOES)image);
    GLuint program = make_program();
    const GLfloat vertices[] = {-1,-1, 1,-1, -1,1, 1,1};
    if (!program) { send_result(client, TRIERARCH_GPU_PROBE_DRAW_FAILED, EGL_SUCCESS); }
    else {
        glViewport(0, 0, buffer.width, buffer.height); glUseProgram(program);
        GLint position = glGetAttribLocation(program, "p");
        glEnableVertexAttribArray((GLuint)position);
        glVertexAttribPointer((GLuint)position, 2, GL_FLOAT, GL_FALSE, 0, vertices);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); glDisableVertexAttribArray((GLuint)position);
        GLenum error = glGetError(); eglSwapBuffers(display, surface);
        send_result(client, error == GL_NO_ERROR ? TRIERARCH_GPU_PROBE_OK :
                TRIERARCH_GPU_PROBE_DRAW_FAILED, EGL_SUCCESS);
        glDeleteProgram(program);
    }
    glDeleteTextures(1, &texture); destroy_image(display, image);
done_client:
    if (buffer_fd >= 0) close(buffer_fd); close(client);
fail_current:
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(display, surface); eglDestroyContext(display, context);
fail_egl:
    eglTerminate(display);
fail:
    unlink(socket_path); close(listener); return 0;
}
