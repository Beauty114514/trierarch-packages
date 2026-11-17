// SPDX-License-Identifier: MIT
//
// Share one GBM device between Aquamarine's allocator and Hyprland's EGL
// fallback on the PRoot fake render node.
//
// Aquamarine creates a GBM device for its output swapchain.  Hyprland then
// creates a second GBM device for its own EGL display.  On the PRoot fake
// render node (KGSL bound as /dev/dri/renderD0) the second gbm_create_device()
// can block forever in Mesa/Turnip.  Reusing the first device is sufficient:
// the two consumers only need an allocator/EGL device, not independent GBM
// instances.
//
// The shim is opt-in through LD_PRELOAD and only touches the synthetic
// renderD0 node.  Non-PRoot sessions and other DRM nodes keep the real libgbm
// path.  Set TRIERARCH_GBM_SINGLE_DEVICE=0 to disable it per process.

#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct gbm_device;

typedef struct gbm_device *(*gbm_create_device_fn)(int fd);
typedef void (*gbm_device_destroy_fn)(struct gbm_device *device);

static gbm_create_device_fn real_create_device;
static gbm_device_destroy_fn real_destroy_device;

static pthread_once_t symbols_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;

static struct gbm_device *shared_device;
static unsigned int shared_references;

static void load_symbols(void) {
    void *symbol = dlsym(RTLD_NEXT, "gbm_create_device");
    memcpy(&real_create_device, &symbol, sizeof(real_create_device));
    symbol = dlsym(RTLD_NEXT, "gbm_device_destroy");
    memcpy(&real_destroy_device, &symbol, sizeof(real_destroy_device));
}

static bool is_fake_render_node(int fd) {
    char path[64];
    char target[256];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    ssize_t length = readlink(path, target, sizeof(target) - 1);
    if (length < 0)
        return false;
    target[length] = '\0';
    return strcmp(target, "/dev/dri/renderD0") == 0;
}

static bool shim_disabled(void) {
    const char *value = getenv("TRIERARCH_GBM_SINGLE_DEVICE");
    return value && strcmp(value, "0") == 0;
}

static void debug_log(const char *message, int fd, struct gbm_device *device) {
    const char *debug = getenv("TRIERARCH_GBM_SINGLE_DEVICE_DEBUG");
    if (!debug || strcmp(debug, "0") == 0)
        return;
    fprintf(stderr, "[trierarch-gbm-single] %s fd=%d device=%p refs=%u\n",
            message, fd, (void *)device, shared_references);
}

struct gbm_device *gbm_create_device(int fd) {
    pthread_once(&symbols_once, load_symbols);
    if (!real_create_device) {
        errno = ENOSYS;
        return NULL;
    }

    if (shim_disabled() || !is_fake_render_node(fd))
        return real_create_device(fd);

    pthread_mutex_lock(&state_lock);
    if (shared_device) {
        shared_references++;
        debug_log("reuse", fd, shared_device);
        pthread_mutex_unlock(&state_lock);
        close(fd);
        return shared_device;
    }

    struct gbm_device *device = real_create_device(fd);
    if (device) {
        shared_device = device;
        shared_references = 1;
        debug_log("create", fd, device);
    }
    pthread_mutex_unlock(&state_lock);
    return device;
}

void gbm_device_destroy(struct gbm_device *device) {
    pthread_once(&symbols_once, load_symbols);
    if (!real_destroy_device) {
        errno = ENOSYS;
        return;
    }

    if (device != shared_device) {
        real_destroy_device(device);
        return;
    }

    pthread_mutex_lock(&state_lock);
    if (device != shared_device) {
        pthread_mutex_unlock(&state_lock);
        real_destroy_device(device);
        return;
    }

    if (shared_references > 0) {
        shared_references--;
        debug_log("release", -1, device);
        if (shared_references > 0) {
            pthread_mutex_unlock(&state_lock);
            return;
        }
    }
    shared_device = NULL;
    pthread_mutex_unlock(&state_lock);
    real_destroy_device(device);
}
