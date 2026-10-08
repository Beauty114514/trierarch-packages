#define _POSIX_C_SOURCE 200809L

#include "dmabuf_device_report_protocol.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <unistd.h>

static int report_device(const char *socket_path, const char *device_path, const struct stat *metadata) {
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("create dmabuf device-report socket");
        return 1;
    }
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if (strlen(socket_path) >= sizeof(address.sun_path)) {
        fprintf(stderr, "dmabuf device-report socket path is too long: %s\n", socket_path);
        close(fd);
        return 1;
    }
    strcpy(address.sun_path, socket_path);
    if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        fprintf(stderr, "connect dmabuf device-report socket %s: %s\n", socket_path, strerror(errno));
        close(fd);
        return 1;
    }
    struct trierarch_dmabuf_device_report report = {
        .magic = TRIERARCH_DMABUF_DEVICE_REPORT_MAGIC,
        .version = TRIERARCH_DMABUF_DEVICE_REPORT_VERSION,
        .major = major(metadata->st_rdev),
        .minor = minor(metadata->st_rdev),
    };
    if (send(fd, &report, sizeof(report), MSG_NOSIGNAL) != (ssize_t)sizeof(report)) {
        fprintf(stderr, "send dmabuf device report: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    printf("reported render node %s as dev_t %u:%u\n", device_path, report.major, report.minor);
    close(fd);
    return 0;
}

static int report_first_render_node(const char *socket_path) {
    DIR *directory = opendir("/dev/dri");
    if (!directory) {
        fprintf(stderr, "open /dev/dri: %s\n", strerror(errno));
        return 1;
    }
    int status = 1;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strncmp(entry->d_name, "renderD", 7) != 0 || entry->d_name[7] == '\0') continue;
        char *end = NULL;
        (void)strtol(entry->d_name + 7, &end, 10);
        if (!end || *end != '\0') continue;
        char path[sizeof("/dev/dri/") + 255];
        int length = snprintf(path, sizeof(path), "/dev/dri/%s", entry->d_name);
        if (length <= 0 || (size_t)length >= sizeof(path)) continue;
        struct stat metadata;
        if (stat(path, &metadata) != 0 || !S_ISCHR(metadata.st_mode)) continue;
        status = report_device(socket_path, path, &metadata);
        break;
    }
    closedir(directory);
    if (status != 0) fprintf(stderr, "no usable /dev/dri/renderD* node found\n");
    return status;
}

int main(int argc, char **argv) {
    if (argc != 3 || strcmp(argv[1], "--socket") != 0) {
        fprintf(stderr, "usage: %s --socket /path/to/dmabuf-feedback.sock\n", argv[0]);
        return 2;
    }
    return report_first_render_node(argv[2]);
}
