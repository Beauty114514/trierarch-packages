#include "compositor.h"
#include "renderer.h"

#include <jni.h>
#include <pthread.h>
#include <android/native_window_jni.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

static pthread_mutex_t server_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t command_mutex = PTHREAD_MUTEX_INITIALIZER;
static wayland_server_t *server;
static pthread_t dispatch_thread;
static bool dispatch_running;
static bool dispatch_thread_started;
/* Owned by the dispatcher once a server exists, or retained here until nativeStart. */
static ANativeWindow *native_window;
static struct renderer_context *renderer;

enum host_command_type {
    HOST_COMMAND_ATTACH_WINDOW,
    HOST_COMMAND_OUTPUT_SIZE,
    HOST_COMMAND_POINTER_ABSOLUTE,
    HOST_COMMAND_POINTER_RELATIVE,
    HOST_COMMAND_POINTER_BUTTON,
    HOST_COMMAND_POINTER_SCROLL,
    HOST_COMMAND_POINTER_RESET,
    HOST_COMMAND_CURSOR_VISIBLE,
};

struct host_command {
    struct host_command *next;
    enum host_command_type type;
    union {
        struct { ANativeWindow *window; } attach_window;
        struct { int width, height; } output_size;
        struct { float x, y; uint32_t time_ms; } pointer_move;
        struct { int button; bool pressed; uint32_t time_ms; } pointer_button;
        struct { float x, y; uint32_t source, time_ms; } pointer_scroll;
        struct { uint32_t time_ms; } pointer_reset;
        struct { bool visible; } cursor_visible;
    } data;
};

static struct host_command *command_head;
static struct host_command *command_tail;

static void discard_commands(void) {
    pthread_mutex_lock(&command_mutex);
    struct host_command *command = command_head;
    command_head = command_tail = NULL;
    pthread_mutex_unlock(&command_mutex);
    while (command) {
        struct host_command *next = command->next;
        if (command->type == HOST_COMMAND_ATTACH_WINDOW && command->data.attach_window.window)
            ANativeWindow_release(command->data.attach_window.window);
        free(command);
        command = next;
    }
}

/* The caller holds server_mutex, which keeps server alive until the wake write. */
static bool enqueue_command_locked(struct host_command *command) {
    if (!server || !dispatch_running || !command) return false;
    pthread_mutex_lock(&command_mutex);
    if (command_tail) command_tail->next = command;
    else command_head = command;
    command_tail = command;
    pthread_mutex_unlock(&command_mutex);
    trierarch_wayland_wake(server);
    return true;
}

static struct host_command *new_command(enum host_command_type type) {
    struct host_command *command = calloc(1, sizeof(*command));
    if (command) command->type = type;
    return command;
}

static void replace_renderer(wayland_server_t *active_server, ANativeWindow *window) {
    trierarch_renderer_destroy(renderer);
    renderer = NULL;
    if (native_window) ANativeWindow_release(native_window);
    native_window = window;
    if (native_window)
        renderer = trierarch_renderer_create(native_window, active_server);
    if (renderer) trierarch_wayland_request_render(active_server);
}

static void process_commands(wayland_server_t *active_server) {
    pthread_mutex_lock(&command_mutex);
    struct host_command *command = command_head;
    command_head = command_tail = NULL;
    pthread_mutex_unlock(&command_mutex);

    while (command) {
        struct host_command *next = command->next;
        switch (command->type) {
        case HOST_COMMAND_ATTACH_WINDOW:
            replace_renderer(active_server, command->data.attach_window.window);
            command->data.attach_window.window = NULL;
            break;
        case HOST_COMMAND_OUTPUT_SIZE:
            trierarch_wayland_set_output_size(active_server,
                    command->data.output_size.width, command->data.output_size.height);
            break;
        case HOST_COMMAND_POINTER_ABSOLUTE:
            trierarch_pointer_move_absolute(active_server, command->data.pointer_move.x,
                    command->data.pointer_move.y, command->data.pointer_move.time_ms);
            break;
        case HOST_COMMAND_POINTER_RELATIVE:
            trierarch_pointer_move_relative(active_server, command->data.pointer_move.x,
                    command->data.pointer_move.y, command->data.pointer_move.time_ms);
            break;
        case HOST_COMMAND_POINTER_BUTTON:
            trierarch_pointer_set_button(active_server, command->data.pointer_button.button,
                    command->data.pointer_button.pressed, command->data.pointer_button.time_ms);
            break;
        case HOST_COMMAND_POINTER_SCROLL:
            trierarch_pointer_scroll(active_server, command->data.pointer_scroll.x,
                    command->data.pointer_scroll.y, command->data.pointer_scroll.source,
                    command->data.pointer_scroll.time_ms);
            break;
        case HOST_COMMAND_POINTER_RESET:
            trierarch_pointer_reset(active_server, command->data.pointer_reset.time_ms);
            break;
        case HOST_COMMAND_CURSOR_VISIBLE:
            trierarch_pointer_set_cursor_visible(active_server,
                    command->data.cursor_visible.visible);
            break;
        }
        free(command);
        command = next;
    }
}

static void *dispatch_loop(void *argument) {
    wayland_server_t *active_server = argument;
    for (;;) {
        trierarch_wayland_dispatch(active_server);
        process_commands(active_server);
        if (renderer && trierarch_wayland_begin_repaint(active_server)) {
            if (!trierarch_renderer_render(renderer, active_server))
                trierarch_wayland_repaint_failed(active_server);
        }
        trierarch_renderer_report_performance(active_server);

        pthread_mutex_lock(&server_mutex);
        bool running = dispatch_running && server == active_server;
        pthread_mutex_unlock(&server_mutex);
        if (!running) break;
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
            dispatch_running = true;
            if (native_window) {
                struct host_command *command = new_command(HOST_COMMAND_ATTACH_WINDOW);
                if (command) {
                    ANativeWindow_acquire(native_window);
                    command->data.attach_window.window = native_window;
                    (void)enqueue_command_locked(command);
                }
            }
            if (pthread_create(&dispatch_thread, NULL, dispatch_loop, server) == 0) {
                dispatch_thread_started = true;
            } else {
                dispatch_running = false;
                discard_commands();
                trierarch_wayland_destroy(server);
                server = NULL;
            }
        }
    }
    bool started = server != NULL;
    pthread_mutex_unlock(&server_mutex);
    (*env)->ReleaseStringUTFChars(env, runtime_directory, runtime);
    return started ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_app_trierarch_wayland_WaylandBridge_nativeStop(JNIEnv *env, jobject object) {
    (void)env;
    (void)object;
    pthread_mutex_lock(&server_mutex);
    dispatch_running = false;
    wayland_server_t *active_server = server;
    bool should_join = dispatch_thread_started;
    if (active_server) trierarch_wayland_wake(active_server);
    pthread_mutex_unlock(&server_mutex);
    if (should_join) pthread_join(dispatch_thread, NULL);

    pthread_mutex_lock(&server_mutex);
    dispatch_thread_started = false;
    discard_commands();
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
    (void)env; (void)object;
    if (width <= 0 || height <= 0) return;
    struct host_command *command = new_command(HOST_COMMAND_OUTPUT_SIZE);
    if (!command) return;
    command->data.output_size.width = width;
    command->data.output_size.height = height;
    pthread_mutex_lock(&server_mutex);
    bool queued = enqueue_command_locked(command);
    pthread_mutex_unlock(&server_mutex);
    if (!queued) free(command);
}

static void enqueue_pointer_move(enum host_command_type type, float x, float y, uint32_t time_ms) {
    struct host_command *command = new_command(type);
    if (!command) return;
    command->data.pointer_move.x = x;
    command->data.pointer_move.y = y;
    command->data.pointer_move.time_ms = time_ms;
    pthread_mutex_lock(&server_mutex);
    bool queued = enqueue_command_locked(command);
    pthread_mutex_unlock(&server_mutex);
    if (!queued) free(command);
}

JNIEXPORT void JNICALL
Java_app_trierarch_wayland_WaylandBridge_nativeMovePointerAbsolute(JNIEnv *env, jobject object,
        jfloat x, jfloat y, jint time_ms) {
    (void)env; (void)object;
    enqueue_pointer_move(HOST_COMMAND_POINTER_ABSOLUTE, x, y, (uint32_t)time_ms);
}

JNIEXPORT void JNICALL
Java_app_trierarch_wayland_WaylandBridge_nativeMovePointerRelative(JNIEnv *env, jobject object,
        jfloat delta_x, jfloat delta_y, jint time_ms) {
    (void)env; (void)object;
    enqueue_pointer_move(HOST_COMMAND_POINTER_RELATIVE, delta_x, delta_y, (uint32_t)time_ms);
}

JNIEXPORT void JNICALL
Java_app_trierarch_wayland_WaylandBridge_nativeSetPointerButton(JNIEnv *env, jobject object,
        jint button, jboolean pressed, jint time_ms) {
    (void)env; (void)object;
    struct host_command *command = new_command(HOST_COMMAND_POINTER_BUTTON);
    if (!command) return;
    command->data.pointer_button.button = button;
    command->data.pointer_button.pressed = pressed == JNI_TRUE;
    command->data.pointer_button.time_ms = (uint32_t)time_ms;
    pthread_mutex_lock(&server_mutex);
    bool queued = enqueue_command_locked(command);
    pthread_mutex_unlock(&server_mutex);
    if (!queued) free(command);
}

JNIEXPORT void JNICALL
Java_app_trierarch_wayland_WaylandBridge_nativeScrollPointer(JNIEnv *env, jobject object,
        jfloat delta_x, jfloat delta_y, jint source, jint time_ms) {
    (void)env; (void)object;
    struct host_command *command = new_command(HOST_COMMAND_POINTER_SCROLL);
    if (!command) return;
    command->data.pointer_scroll.x = delta_x;
    command->data.pointer_scroll.y = delta_y;
    command->data.pointer_scroll.source = (uint32_t)source;
    command->data.pointer_scroll.time_ms = (uint32_t)time_ms;
    pthread_mutex_lock(&server_mutex);
    bool queued = enqueue_command_locked(command);
    pthread_mutex_unlock(&server_mutex);
    if (!queued) free(command);
}

JNIEXPORT void JNICALL
Java_app_trierarch_wayland_WaylandBridge_nativeResetPointer(JNIEnv *env, jobject object,
        jint time_ms) {
    (void)env; (void)object;
    struct host_command *command = new_command(HOST_COMMAND_POINTER_RESET);
    if (!command) return;
    command->data.pointer_reset.time_ms = (uint32_t)time_ms;
    pthread_mutex_lock(&server_mutex);
    bool queued = enqueue_command_locked(command);
    pthread_mutex_unlock(&server_mutex);
    if (!queued) free(command);
}

JNIEXPORT void JNICALL
Java_app_trierarch_wayland_WaylandBridge_nativeSetCursorVisible(JNIEnv *env, jobject object,
        jboolean visible) {
    (void)env; (void)object;
    struct host_command *command = new_command(HOST_COMMAND_CURSOR_VISIBLE);
    if (!command) return;
    command->data.cursor_visible.visible = visible == JNI_TRUE;
    pthread_mutex_lock(&server_mutex);
    bool queued = enqueue_command_locked(command);
    pthread_mutex_unlock(&server_mutex);
    if (!queued) free(command);
}

JNIEXPORT void JNICALL
Java_app_trierarch_wayland_WaylandBridge_nativeAttachSurface(JNIEnv *env, jobject object,
        jobject surface) {
    (void)object;
    ANativeWindow *window = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
    pthread_mutex_lock(&server_mutex);
    if (!server || !dispatch_running) {
        if (native_window) ANativeWindow_release(native_window);
        native_window = window;
        pthread_mutex_unlock(&server_mutex);
        return;
    }
    struct host_command *command = new_command(HOST_COMMAND_ATTACH_WINDOW);
    if (command) command->data.attach_window.window = window;
    bool queued = enqueue_command_locked(command);
    pthread_mutex_unlock(&server_mutex);
    if (!queued) {
        if (window) ANativeWindow_release(window);
        free(command);
    }
}
