// SPDX-License-Identifier: MIT
//
// Report PRIME import/export capability for the PRoot fake render node so
// Aquamarine reaches its GBM allocator constructor. This is diagnostic: it does
// not change the real buffer export path.

#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define DRM_CAP_PRIME 0x5
#define DRM_PRIME_CAP_IMPORT 0x1
#define DRM_PRIME_CAP_EXPORT 0x2

typedef int (*drm_get_cap_fn)(int fd, uint64_t capability, uint64_t *value);

static drm_get_cap_fn real_get_cap;

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

static void log_cap(int fd, uint64_t capability, uint64_t value) {
    char line[256];
    int length = snprintf(line, sizeof(line),
                          "drmGetCap fd=%d capability=0x%llx value=0x%llx "
                          "source=fake-prime\n",
                          fd, (unsigned long long)capability,
                          (unsigned long long)value);
    if (length <= 0)
        return;

    int log_fd = open("/tmp/trierarch-drm-prime-probe.log",
                      O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
    if (log_fd < 0)
        return;
    ssize_t ignored = write(log_fd, line, (size_t)length);
    (void)ignored;
    close(log_fd);
}

int drmGetCap(int fd, uint64_t capability, uint64_t *value) {
    if (capability == DRM_CAP_PRIME && value && fd_target_is_fake_render_node(fd)) {
        *value = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT;
        log_cap(fd, capability, *value);
        return 0;
    }

    if (!real_get_cap) {
        void *symbol = dlsym(RTLD_NEXT, "drmGetCap");
        memcpy(&real_get_cap, &symbol, sizeof(real_get_cap));
    }

    if (!real_get_cap) {
        errno = ENOSYS;
        return -ENOSYS;
    }

    return real_get_cap(fd, capability, value);
}
