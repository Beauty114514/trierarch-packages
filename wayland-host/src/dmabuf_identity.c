#include "dmabuf_identity.h"

#include <android/log.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TAG "TrierarchDmaBuf"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

static bool relevant_fdinfo_line(const char *line) {
    return strncmp(line, "pos:", 4) == 0 || strncmp(line, "flags:", 6) == 0 ||
            strncmp(line, "mnt_id:", 7) == 0 || strncmp(line, "ino:", 4) == 0 ||
            strncmp(line, "exp_name:", 9) == 0 || strncmp(line, "size:", 5) == 0 ||
            strncmp(line, "count:", 6) == 0;
}

void trierarch_dmabuf_log_process_identity(void) {
    static bool logged;
    if (logged)
        return;
    logged = true;
    char context[256] = {0};
    int attribute = open("/proc/self/attr/current", O_RDONLY | O_CLOEXEC);
    if (attribute >= 0) {
        ssize_t bytes = read(attribute, context, sizeof(context) - 1);
        close(attribute);
        if (bytes > 0)
            context[strcspn(context, "\n")] = '\0';
    }
    LOGI("host identity: uid=%u gid=%u selinux=%s", (unsigned)getuid(),
            (unsigned)getgid(), context[0] ? context : "unavailable");
}

void trierarch_dmabuf_log_identity(const char *label, int fd) {
    if (!label || fd < 0)
        return;
    struct stat status = {0};
    if (fstat(fd, &status) != 0) {
        LOGW("dma-buf %s: fstat failed: %s", label, strerror(errno));
        return;
    }
    char link_path[64];
    char link_target[256] = {0};
    snprintf(link_path, sizeof(link_path), "/proc/self/fd/%d", fd);
    ssize_t link_size = readlink(link_path, link_target, sizeof(link_target) - 1);
    if (link_size >= 0)
        link_target[link_size] = '\0';
    LOGI("dma-buf %s: fd=%d dev=%llu ino=%llu size=%lld link=%s", label, fd,
            (unsigned long long)status.st_dev, (unsigned long long)status.st_ino,
            (long long)status.st_size, link_size >= 0 ? link_target : "unavailable");

    char info_path[64];
    snprintf(info_path, sizeof(info_path), "/proc/self/fdinfo/%d", fd);
    FILE *info = fopen(info_path, "r");
    if (!info) {
        LOGW("dma-buf %s: fdinfo unavailable: %s", label, strerror(errno));
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), info)) {
        if (relevant_fdinfo_line(line))
            LOGI("dma-buf %s fdinfo: %s", label, line);
    }
    fclose(info);
}
