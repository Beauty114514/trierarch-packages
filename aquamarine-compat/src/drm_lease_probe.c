// SPDX-License-Identifier: MIT
//
// Diagnostic LD_PRELOAD shim for drmModeCreateLease on the PRoot fake render
// node. It intentionally preserves the real return value and errno.

#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef int (*drm_mode_create_lease_fn)(int fd, const uint32_t *objects,
                                        int num_objects, int flags,
                                        uint32_t *lessee_id);

static drm_mode_create_lease_fn real_create_lease;

static void log_call(int fd, const uint32_t *objects, int num_objects, int flags,
                     int result, int error) {
    char target[256] = "?";
    char fd_path[64];
    snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", fd);
    ssize_t target_len = readlink(fd_path, target, sizeof(target) - 1);
    if (target_len >= 0)
        target[target_len] = '\0';
    else
        strcpy(target, "?");

    char line[768];
    int length = snprintf(line, sizeof(line),
                          "drmModeCreateLease fd=%d target=%s objects=%p count=%d "
                          "flags=%d result=%d errno=%d (%s)\n",
                          fd, target, (const void *)objects, num_objects, flags,
                          result, error, strerror(error));
    if (length <= 0)
        return;

    int log_fd = open("/tmp/trierarch-drm-lease-probe.log",
                      O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
    if (log_fd < 0)
        return;

    ssize_t ignored = write(log_fd, line, (size_t)length);
    (void)ignored;
    close(log_fd);
}

int drmModeCreateLease(int fd, const uint32_t *objects, int num_objects,
                       int flags, uint32_t *lessee_id) {
    if (!real_create_lease) {
        void *symbol = dlsym(RTLD_NEXT, "drmModeCreateLease");
        memcpy(&real_create_lease, &symbol, sizeof(real_create_lease));
    }

    if (!real_create_lease) {
        errno = ENOSYS;
        log_call(fd, objects, num_objects, flags, -1, errno);
        return -1;
    }

    errno = 0;
    int result = real_create_lease(fd, objects, num_objects, flags, lessee_id);
    int error = errno;
    log_call(fd, objects, num_objects, flags, result, error);
    errno = error;
    return result;
}
