#define _GNU_SOURCE
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <android/surface_control.h>
#include <jni.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define TAG "TrierarchScanoutTest"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

struct transaction_state {
    pthread_mutex_t lock;
    pthread_cond_t condition;
    bool complete;
};

static void transaction_complete(void *context, ASurfaceTransactionStats *stats) {
    (void)stats;
    struct transaction_state *state = context;
    pthread_mutex_lock(&state->lock);
    state->complete = true;
    pthread_cond_signal(&state->condition);
    pthread_mutex_unlock(&state->lock);
}

static bool fill_buffer(AHardwareBuffer *buffer, uint32_t width, uint32_t height) {
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
            row[x * 4 + 0] = 255;
            row[x * 4 + 1] = 0;
            row[x * 4 + 2] = 0;
            row[x * 4 + 3] = 255;
        }
    }
    result = AHardwareBuffer_unlock(buffer, NULL);
    if (result != 0) LOGE("AHardwareBuffer_unlock failed: %d", result);
    return result == 0;
}

static int apply_scanout(ANativeWindow *window) {
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
    if (!fill_buffer(buffer, width, height)) {
        AHardwareBuffer_release(buffer);
        return 1;
    }

    ASurfaceControl *control = ASurfaceControl_createFromWindow(window, "trierarch-scanout-test");
    if (!control) {
        LOGE("ASurfaceControl_createFromWindow failed");
        AHardwareBuffer_release(buffer);
        return 1;
    }
    ASurfaceTransaction *transaction = ASurfaceTransaction_create();
    struct transaction_state state = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .condition = PTHREAD_COND_INITIALIZER,
    };
    ASurfaceTransaction_setVisibility(transaction, control, ASURFACE_TRANSACTION_VISIBILITY_SHOW);
    ASurfaceTransaction_setBuffer(transaction, control, buffer, -1);
    ASurfaceTransaction_setOnComplete(transaction, &state, transaction_complete);
    ASurfaceTransaction_apply(transaction);

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5;
    pthread_mutex_lock(&state.lock);
    while (!state.complete) {
        if (pthread_cond_timedwait(&state.condition, &state.lock, &deadline) != 0) break;
    }
    bool complete = state.complete;
    pthread_mutex_unlock(&state.lock);
    LOGI("transaction complete=%s", complete ? "true" : "false");

    ASurfaceTransaction_delete(transaction);
    ASurfaceControl_release(control);
    AHardwareBuffer_release(buffer);
    return complete ? 0 : 1;
}

JNIEXPORT jint JNICALL
Java_app_trierarch_devicetest_MainActivity_runScanout(JNIEnv *env, jobject thiz, jobject surface) {
    (void)thiz;
    ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
    if (!window) {
        LOGE("ANativeWindow_fromSurface failed");
        return 1;
    }
    int result = apply_scanout(window);
    ANativeWindow_release(window);
    return result;
}
