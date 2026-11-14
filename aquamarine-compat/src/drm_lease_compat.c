// SPDX-License-Identifier: MIT
//
// Translate DRM lease failure on the PRoot fake render node into the
// -EOPNOTSUPP result that Aquamarine understands as "fall back to opening the
// node directly".

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

static int fd_target_is_fake_render_node(int fd) {
    char path[64];
    char target[256];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);

    ssize_t length = readlink(path, target, sizeof(target) - 1);
    if (length < 0)
        return 0;
    target[length] = '\0';
    return strcmp(target, "/dev/dri/renderD0") == 0;
}

static void log_translation(int fd, int result) {
    char path[64];
    char target[256] = "?";
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);

    ssize_t length = readlink(path, target, sizeof(target) - 1);
    if (length >= 0)
        target[length] = '\0';

    char line[384];
    int line_length = snprintf(line, sizeof(line),
                               "drmModeCreateLease fd=%d target=%s "
                               "real_result=%d translated=%d\n",
                               fd, target, result, -EOPNOTSUPP);
    if (line_length <= 0)
        return;

    int log_fd = open("/tmp/trierarch-drm-lease-probe.log",
                      O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
    if (log_fd < 0)
        return;
    ssize_t ignored = write(log_fd, line, (size_t)line_length);
    (void)ignored;
    close(log_fd);
}

int drmModeCreateLease(int fd, const uint32_t *objects, int num_objects,
                       int flags, uint32_t *lessee_id) {
    if (fd_target_is_fake_render_node(fd)) {
        log_translation(fd, -ENOTTY);
        errno = EOPNOTSUPP;
        return -EOPNOTSUPP;
    }

    if (!real_create_lease) {
        void *symbol = dlsym(RTLD_NEXT, "drmModeCreateLease");
        memcpy(&real_create_lease, &symbol, sizeof(real_create_lease));
    }

    if (!real_create_lease) {
        errno = ENOSYS;
        return -ENOSYS;
    }

    return real_create_lease(fd, objects, num_objects, flags, lessee_id);
}
