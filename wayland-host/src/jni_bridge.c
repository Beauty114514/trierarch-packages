#include "compositor.h"
#include "renderer.h"

#include <jni.h>
#include <pthread.h>
#include <android/native_window_jni.h>
#include <unistd.h>

static pthread_mutex_t server_mutex = PTHREAD_MUTEX_INITIALIZER;
static wayland_server_t *server;
static pthread_t dispatch_thread;
static int dispatch_running;
static ANativeWindow *native_window;
static struct renderer_context *renderer;

static void *dispatch_loop(void *argument) {
    (void)argument;
    for (;;) {
        pthread_mutex_lock(&server_mutex);
        if (!dispatch_running || !server) {
            pthread_mutex_unlock(&server_mutex);
            break;
        }
        trierarch_wayland_dispatch(server);
        if (renderer) trierarch_renderer_render(renderer, server);
        pthread_mutex_unlock(&server_mutex);
    }
    return NULL;
}

JNIEXPORT jboolean JNICALL
Java_app_trierarch_wayland_WaylandBridge_nativeStart(JNIEnv *env, jobject object,
        jstring runtime_directory) {
    (void)object;
    const char *runtime = (*env)->GetStringUTFChars(env, runtime_directory, NULL);
    if (!runtime) return JNI_FALSE;
    pthread_mutex_lock(&server_mutex);
    if (!server) {
        server = trierarch_wayland_create(runtime);
        if (server) {
            /* SurfaceView may have been created before the runtime starts.
             * In that order nativeAttachSurface stores native_window while
             * server is NULL, so create the renderer here as well. */
            if (native_window)
                renderer = trierarch_renderer_create(native_window, server);
            dispatch_running = 1;
            if (pthread_create(&dispatch_thread, NULL, dispatch_loop, NULL) != 0) {
                dispatch_running = 0;
                trierarch_renderer_destroy(renderer);
                renderer = NULL;
                trierarch_wayland_destroy(server);
                server = NULL;
            }
        }
    }
    pthread_mutex_unlock(&server_mutex);
    (*env)->ReleaseStringUTFChars(env, runtime_directory, runtime);
    return server != NULL ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_app_trierarch_wayland_WaylandBridge_nativeStop(JNIEnv *env, jobject object) {
    (void)env;
    (void)object;
    pthread_mutex_lock(&server_mutex);
    dispatch_running = 0;
    int should_join = server != NULL;
    pthread_mutex_unlock(&server_mutex);
    if (should_join) pthread_join(dispatch_thread, NULL);

    pthread_mutex_lock(&server_mutex);
    trierarch_renderer_destroy(renderer);
    renderer = NULL;
    trierarch_wayland_destroy(server);
    server = NULL;
    if (native_window) {
        ANativeWindow_release(native_window);
        native_window = NULL;
    }
    pthread_mutex_unlock(&server_mutex);
}

JNIEXPORT void JNICALL
Java_app_trierarch_wayland_WaylandBridge_nativeSetOutputSize(JNIEnv *env, jobject object,
        jint width, jint height) {
    (void)env;
    (void)object;
    pthread_mutex_lock(&server_mutex);
    if (server && width > 0 && height > 0)
        trierarch_wayland_set_output_size(server, width, height);
    pthread_mutex_unlock(&server_mutex);
}

JNIEXPORT void JNICALL
Java_app_trierarch_wayland_WaylandBridge_nativeAttachSurface(JNIEnv *env, jobject object,
        jobject surface) {
    (void)object;
    ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
    pthread_mutex_lock(&server_mutex);
    if (native_window) ANativeWindow_release(native_window);
    trierarch_renderer_destroy(renderer);
    renderer = NULL;
    native_window = window;
    if (native_window && server)
        renderer = trierarch_renderer_create(native_window, server);
    pthread_mutex_unlock(&server_mutex);
}
