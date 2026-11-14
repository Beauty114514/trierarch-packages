// SPDX-License-Identifier: MIT
//
// Return a valid DRM device name for the PRoot fake render node. libdrm's
// sysfs lookup returns NULL for the synthetic node, and Aquamarine passes that
// NULL into std::string, which crashes in strlen.

#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef char *(*drm_get_device_name_fn)(int fd);

static drm_get_device_name_fn real_get_device_name;

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

static void log_fallback(int fd) {
    char line[192];
    int length = snprintf(line, sizeof(line),
                          "drmGetDeviceNameFromFd2 fd=%d source=fake-render-node\n",
                          fd);
    if (length <= 0)
        return;

    int log_fd = open("/tmp/trierarch-drm-device-name-probe.log",
                      O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
    if (log_fd < 0)
        return;
    ssize_t ignored = write(log_fd, line, (size_t)length);
    (void)ignored;
    close(log_fd);
}

char *drmGetDeviceNameFromFd2(int fd) {
    if (fd_target_is_fake_render_node(fd)) {
        log_fallback(fd);
        return strdup("/dev/dri/renderD0");
    }

    if (!real_get_device_name) {
        void *symbol = dlsym(RTLD_NEXT, "drmGetDeviceNameFromFd2");
        memcpy(&real_get_device_name, &symbol, sizeof(real_get_device_name));
    }

    if (!real_get_device_name) {
        errno = ENOSYS;
        return NULL;
    }

    return real_get_device_name(fd);
}
