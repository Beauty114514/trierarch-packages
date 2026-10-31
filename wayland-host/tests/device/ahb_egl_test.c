#define _GNU_SOURCE
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "TrierarchAhbEgl"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#ifndef EGL_NATIVE_BUFFER_ANDROID
#define EGL_NATIVE_BUFFER_ANDROID 0x3140
#endif
#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x0040
#endif

typedef EGLClientBuffer (*GetNativeClientBufferProc)(const AHardwareBuffer *buffer);
typedef EGLImageKHR (*CreateImageProc)(EGLDisplay, EGLContext, EGLenum,
        EGLClientBuffer, const EGLint *);
typedef EGLBoolean (*DestroyImageProc)(EGLDisplay, EGLImageKHR);
typedef GLboolean (*ImageTargetTextureProc)(GLenum, GLeglImageOES);

static bool compile_shader(GLenum type, const char *source, GLuint *out) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        LOGE("shader compile failed: %s", log);
        glDeleteShader(shader);
        return false;
    }
    *out = shader;
    return true;
}

static bool build_program(GLuint *out_program) {
    static const char *vertex_source =
        "attribute vec2 position;\n"
        "varying vec2 uv;\n"
        "void main() {\n"
        "  uv = position * 0.5 + 0.5;\n"
        "  gl_Position = vec4(position, 0.0, 1.0);\n"
        "}\n";
    static const char *fragment_source =
        "#extension GL_OES_EGL_image_external : require\n"
        "precision mediump float;\n"
        "varying vec2 uv;\n"
        "uniform samplerExternalOES image;\n"
        "void main() { gl_FragColor = texture2D(image, uv); }\n";
    GLuint vertex = 0, fragment = 0;
    if (!compile_shader(GL_VERTEX_SHADER, vertex_source, &vertex)) return false;
    if (!compile_shader(GL_FRAGMENT_SHADER, fragment_source, &fragment)) {
        glDeleteShader(vertex);
        return false;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glBindAttribLocation(program, 0, "position");
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        LOGE("program link failed: %s", log);
        glDeleteProgram(program);
        return false;
    }
    *out_program = program;
    return true;
}

static bool fill_ahb_green(AHardwareBuffer *buffer, uint32_t width, uint32_t height) {
    void *data = NULL;
    int result = AHardwareBuffer_lock(buffer,
            AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &data);
    if (result != 0 || !data) {
        LOGE("AHardwareBuffer_lock failed: %d", result);
        return false;
    }
    AHardwareBuffer_Desc desc;
    AHardwareBuffer_describe(buffer, &desc);
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t *row = (uint8_t *)data + (size_t)y * desc.stride * 4;
        for (uint32_t x = 0; x < width; ++x) {
            row[x * 4 + 0] = 0;
            row[x * 4 + 1] = 255;
            row[x * 4 + 2] = 0;
            row[x * 4 + 3] = 255;
        }
    }
    result = AHardwareBuffer_unlock(buffer, NULL);
    if (result != 0) LOGE("AHardwareBuffer_unlock failed: %d", result);
    return result == 0;
}

static int run_test(void) {
    const uint32_t width = 256, height = 256;
    AHardwareBuffer_Desc desc = {
        .width = width,
        .height = height,
        .layers = 1,
        .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
        .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                 AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
    };
    AHardwareBuffer *buffer = NULL;
    int result = AHardwareBuffer_allocate(&desc, &buffer);
    if (result != 0 || !buffer) {
        LOGE("AHardwareBuffer_allocate failed: %d", result);
        return 1;
    }
    LOGI("allocated AHB %ux%u format=%u stride=%u", width, height,
            desc.format, desc.stride);
    if (!fill_ahb_green(buffer, width, height)) {
        AHardwareBuffer_release(buffer);
        return 1;
    }

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL)) {
        LOGE("eglInitialize failed: 0x%x", eglGetError());
        AHardwareBuffer_release(buffer);
        return 1;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        LOGE("eglBindAPI failed: 0x%x", eglGetError());
        eglTerminate(display);
        AHardwareBuffer_release(buffer);
        return 1;
    }
    const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig config = NULL;
    EGLint config_count = 0;
    if (!eglChooseConfig(display, config_attributes, &config, 1, &config_count) ||
            config_count == 0) {
        LOGE("eglChooseConfig failed: 0x%x", eglGetError());
        eglTerminate(display);
        AHardwareBuffer_release(buffer);
        return 1;
    }
    const EGLint pbuffer_attributes[] = {
        EGL_WIDTH, (EGLint)width, EGL_HEIGHT, (EGLint)height, EGL_NONE,
    };
    EGLSurface surface = eglCreatePbufferSurface(display, config, pbuffer_attributes);
    if (surface == EGL_NO_SURFACE) {
        LOGE("eglCreatePbufferSurface failed: 0x%x", eglGetError());
        eglTerminate(display);
        AHardwareBuffer_release(buffer);
        return 1;
    }
    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, surface, surface, context)) {
        LOGE("EGL context setup failed: 0x%x", eglGetError());
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        AHardwareBuffer_release(buffer);
        return 1;
    }
    LOGI("EGL %s / GL %s", eglQueryString(display, EGL_VERSION),
            glGetString(GL_VERSION));

    GetNativeClientBufferProc get_native = (GetNativeClientBufferProc)
            eglGetProcAddress("eglGetNativeClientBufferANDROID");
    CreateImageProc create_image = (CreateImageProc)
            eglGetProcAddress("eglCreateImageKHR");
    DestroyImageProc destroy_image = (DestroyImageProc)
            eglGetProcAddress("eglDestroyImageKHR");
    ImageTargetTextureProc target_texture = (ImageTargetTextureProc)
            eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!get_native || !create_image || !destroy_image || !target_texture) {
        LOGE("required EGL/GLES extensions are missing");
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        AHardwareBuffer_release(buffer);
        return 1;
    }

    EGLClientBuffer client_buffer = get_native(buffer);
    EGLImageKHR image = create_image(display, EGL_NO_CONTEXT,
            EGL_NATIVE_BUFFER_ANDROID, client_buffer, NULL);
    if (image == EGL_NO_IMAGE_KHR) {
        LOGE("eglCreateImageKHR failed: 0x%x", eglGetError());
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        AHardwareBuffer_release(buffer);
        return 1;
    }

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
    target_texture(GL_TEXTURE_EXTERNAL_OES, image);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    GLuint program = 0;
    if (!build_program(&program)) {
        glDeleteTextures(1, &texture);
        destroy_image(display, image);
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        AHardwareBuffer_release(buffer);
        return 1;
    }
    glUseProgram(program);
    glViewport(0, 0, (GLsizei)width, (GLsizei)height);
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    static const GLfloat vertices[] = {
        -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f,
        -1.0f, 1.0f, 1.0f, -1.0f, 1.0f, 1.0f,
    };
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glFinish();

    uint8_t pixel[4] = {0};
    glReadPixels((GLint)width / 2, (GLint)height / 2, 1, 1, GL_RGBA,
            GL_UNSIGNED_BYTE, pixel);
    LOGI("center pixel: r=%u g=%u b=%u a=%u", pixel[0], pixel[1], pixel[2], pixel[3]);
    bool ok = pixel[1] > 240 && pixel[0] < 15 && pixel[2] < 15;

    glDeleteProgram(program);
    glDeleteTextures(1, &texture);
    destroy_image(display, image);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, surface);
    eglTerminate(display);
    AHardwareBuffer_release(buffer);
    return ok ? 0 : 1;
}

int main(void) {
    int result = run_test();
    if (result == 0) {
        LOGI("AHB/EGL readback test passed");
        printf("AHB/EGL readback test passed\n");
    } else {
        LOGE("AHB/EGL readback test failed");
        printf("AHB/EGL readback test failed\n");
    }
    return result;
}
