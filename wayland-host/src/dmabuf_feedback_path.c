#include "dmabuf_feedback_path.h"

#include <stdio.h>
#include <unistd.h>

#define FEEDBACK_SOCKET_NAME "dmabuf-feedback.sock"

static int feedback_path(char *path, size_t capacity, const char *runtime_dir) {
    if (!path || capacity == 0 || !runtime_dir || runtime_dir[0] == '\0') return -1;
    int length = snprintf(path, capacity, "%s/%s", runtime_dir, FEEDBACK_SOCKET_NAME);
    return length > 0 && (size_t)length < capacity ? 0 : -1;
}

int trierarch_clear_dmabuf_feedback_socket(const char *runtime_dir) {
    char path[4096];
    if (feedback_path(path, sizeof(path), runtime_dir) != 0) return -1;
    unlink(path);
    return 0;
}

int trierarch_dmabuf_feedback_prepare(const char *runtime_dir, bool enabled) {
    if (!runtime_dir || runtime_dir[0] == '\0') return -1;
    if (enabled) return 0;
    return trierarch_clear_dmabuf_feedback_socket(runtime_dir);
}
