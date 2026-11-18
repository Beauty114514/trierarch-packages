// SPDX-License-Identifier: MIT
//
// Combined Aquamarine compatibility shim for PRoot Hyprland.
//
// The PRoot guest exposes /dev/kgsl-3d0 as the synthetic render node
// /dev/dri/renderD0.  Aquamarine probes that node in four places that assume a
// real DRM/GBM device:
//
//   1. drmGetDeviceNameFromFd2() returns NULL for the synthetic sysfs node.
//   2. drmGetCap(DRM_CAP_PRIME) does not report import/export capability.
//   3. drmModeCreateLease() returns -ENOTTY, which Aquamarine does not treat as
//      an unsupported/fallback result.
//   4. Hyprland creates a second gbm_device() for its EGL fallback; on the
//      fake KGSL node that second creation can block forever.
//   5. Hyprland's LinuxDMABUF commence() validates every plane with
//      drmPrimeFDToHandle() before its own EGLImage import path.  The fake
//      KGSL node rejects that ioctl, so Hyprland sends wl_buffer.failed before
//      the EGL import can run.
//
// This library answers the first three probes and the PRIME validation for the
// synthetic node only and makes the second GBM creation reuse the first
// device.  Every other fd keeps the real libdrm/libgbm behaviour.  It is
// opt-in through LD_PRELOAD and does not touch normal rooted DRM sessions.

#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DRM_CAP_PRIME 0x5
#define DRM_PRIME_CAP_IMPORT 0x1
#define DRM_PRIME_CAP_EXPORT 0x2

struct gbm_device;

typedef char *(*drm_get_device_name_fn)(int fd);
typedef int (*drm_get_cap_fn)(int fd, uint64_t capability, uint64_t *value);
typedef int (*drm_mode_create_lease_fn)(int fd, const uint32_t *objects,
                                        int num_objects, unsigned int flags,
                                        uint32_t *lessee_id);
typedef int (*drm_prime_fd_to_handle_fn)(int fd, int prime_fd,
                                         uint32_t *handle);
typedef int (*drm_close_buffer_handle_fn)(int fd, uint32_t handle);
typedef struct gbm_device *(*gbm_create_device_fn)(int fd);
typedef void (*gbm_device_destroy_fn)(struct gbm_device *device);

static drm_get_device_name_fn real_get_device_name;
static drm_get_cap_fn real_get_cap;
static drm_mode_create_lease_fn real_create_lease;
static drm_prime_fd_to_handle_fn real_prime_fd_to_handle;
static drm_close_buffer_handle_fn real_close_buffer_handle;
static gbm_create_device_fn real_create_device;
static gbm_device_destroy_fn real_destroy_device;

static pthread_once_t symbols_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t gbm_lock = PTHREAD_MUTEX_INITIALIZER;

static struct gbm_device *shared_gbm_device;
static unsigned int shared_gbm_references;

static void load_symbols(void) {
    void *symbol;
#define LOAD(field, name)                                  \
    do {                                                   \
        symbol = dlsym(RTLD_NEXT, name);                   \
        memcpy(&field, &symbol, sizeof(field));            \
    } while (0)
    LOAD(real_get_device_name, "drmGetDeviceNameFromFd2");
    LOAD(real_get_cap, "drmGetCap");
    LOAD(real_create_lease, "drmModeCreateLease");
    LOAD(real_prime_fd_to_handle, "drmPrimeFDToHandle");
    LOAD(real_close_buffer_handle, "drmCloseBufferHandle");
    LOAD(real_create_device, "gbm_create_device");
    LOAD(real_destroy_device, "gbm_device_destroy");
#undef LOAD
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

static bool fake_prime_import_enabled(void) {
    const char *enabled = getenv("TRIERARCH_FAKE_PRIME_IMPORT");
    return enabled && strcmp(enabled, "0") != 0;
}

static void debug_log(const char *message, int fd) {
    const char *debug = getenv("TRIERARCH_AQUAMARINE_COMPAT_DEBUG");
    if (!debug || strcmp(debug, "0") == 0)
        return;
    fprintf(stderr, "[trierarch-aquamarine-compat] %s fd=%d\n", message, fd);
}

char *drmGetDeviceNameFromFd2(int fd) {
    pthread_once(&symbols_once, load_symbols);
    if (is_fake_render_node(fd)) {
        debug_log("report render node name", fd);
        return strdup("/dev/dri/renderD0");
    }
    if (!real_get_device_name) {
        errno = ENOSYS;
        return NULL;
    }
    return real_get_device_name(fd);
}

int drmGetCap(int fd, uint64_t capability, uint64_t *value) {
    pthread_once(&symbols_once, load_symbols);
    if (capability == DRM_CAP_PRIME && value && is_fake_render_node(fd)) {
        *value = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT;
        debug_log("report PRIME import/export", fd);
        return 0;
    }
    if (!real_get_cap) {
        errno = ENOSYS;
        return -ENOSYS;
    }
    return real_get_cap(fd, capability, value);
}

int drmModeCreateLease(int fd, const uint32_t *objects, int num_objects,
                       unsigned int flags, uint32_t *lessee_id) {
    pthread_once(&symbols_once, load_symbols);
    if (is_fake_render_node(fd)) {
        debug_log("translate lease failure to EOPNOTSUPP", fd);
        errno = EOPNOTSUPP;
        return -EOPNOTSUPP;
    }
    if (!real_create_lease) {
        errno = ENOSYS;
        return -ENOSYS;
    }
    return real_create_lease(fd, objects, num_objects, flags, lessee_id);
}

int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle) {
    pthread_once(&symbols_once, load_symbols);
    if (handle && fake_prime_import_enabled() && is_fake_render_node(fd)) {
        /*
         * Hyprland only uses this call as a validation gate in
         * CLinuxDMABUFParamsResource::commence(); it closes the handle
         * immediately and never renders through it.  The renderer's real
         * EGLImage import happens afterwards in OpenGL.cpp.  Return a
         * synthetic handle so the EGLImage path gets a chance to run.
         */
        *handle = 1;
        debug_log("fake PRIME fd-to-handle import", fd);
        return 0;
    }
    if (!real_prime_fd_to_handle) {
        errno = ENOSYS;
        return -ENOSYS;
    }
    return real_prime_fd_to_handle(fd, prime_fd, handle);
}

int drmCloseBufferHandle(int fd, uint32_t handle) {
    pthread_once(&symbols_once, load_symbols);
    if (fake_prime_import_enabled() && is_fake_render_node(fd))
        return 0;
    if (!real_close_buffer_handle) {
        errno = ENOSYS;
        return -ENOSYS;
    }
    return real_close_buffer_handle(fd, handle);
}

struct gbm_device *gbm_create_device(int fd) {
    pthread_once(&symbols_once, load_symbols);
    if (!real_create_device) {
        errno = ENOSYS;
        return NULL;
    }
    if (!is_fake_render_node(fd))
        return real_create_device(fd);

    pthread_mutex_lock(&gbm_lock);
    if (shared_gbm_device) {
        shared_gbm_references++;
        debug_log("reuse shared GBM device", fd);
        pthread_mutex_unlock(&gbm_lock);
        close(fd);
        return shared_gbm_device;
    }

    struct gbm_device *device = real_create_device(fd);
    if (device) {
        shared_gbm_device = device;
        shared_gbm_references = 1;
        debug_log("created shared GBM device", fd);
    }
    pthread_mutex_unlock(&gbm_lock);
    return device;
}

void gbm_device_destroy(struct gbm_device *device) {
    pthread_once(&symbols_once, load_symbols);
    if (!real_destroy_device) {
        errno = ENOSYS;
        return;
    }

    if (device != shared_gbm_device) {
        real_destroy_device(device);
        return;
    }

    pthread_mutex_lock(&gbm_lock);
    if (device != shared_gbm_device) {
        pthread_mutex_unlock(&gbm_lock);
        real_destroy_device(device);
        return;
    }
    if (shared_gbm_references > 0)
        shared_gbm_references--;
    if (shared_gbm_references > 0) {
        debug_log("defer shared GBM destroy", -1);
        pthread_mutex_unlock(&gbm_lock);
        return;
    }
    shared_gbm_device = NULL;
    pthread_mutex_unlock(&gbm_lock);
    real_destroy_device(device);
}
