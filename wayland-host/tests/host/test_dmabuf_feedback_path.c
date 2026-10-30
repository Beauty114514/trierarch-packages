#define _GNU_SOURCE
#include "dmabuf_feedback_path.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void make_stale_socket(const char *runtime_dir) {
    char path[4096];
    int length = snprintf(path, sizeof(path), "%s/dmabuf-feedback.sock", runtime_dir);
    assert(length > 0 && (size_t)length < sizeof(path));
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    assert(fd >= 0);
    close(fd);
}

static int exists(const char *runtime_dir) {
    char path[4096];
    int length = snprintf(path, sizeof(path), "%s/dmabuf-feedback.sock", runtime_dir);
    assert(length > 0 && (size_t)length < sizeof(path));
    return access(path, F_OK) == 0;
}

int main(void) {
    char template[] = "/tmp/trierarch-wayland-feedback-XXXXXX";
    char *runtime_dir = mkdtemp(template);
    assert(runtime_dir != NULL);

    /* A shm session must remove a stale dmabuf socket and must not recreate it. */
    make_stale_socket(runtime_dir);
    assert(exists(runtime_dir));
    assert(trierarch_dmabuf_feedback_prepare(runtime_dir, false) == 0);
    assert(!exists(runtime_dir));
    assert(!exists(runtime_dir));

    /* A dmabuf session leaves an existing socket for the real listener. */
    make_stale_socket(runtime_dir);
    assert(trierarch_dmabuf_feedback_prepare(runtime_dir, true) == 0);
    assert(exists(runtime_dir));

    assert(trierarch_dmabuf_feedback_prepare(NULL, false) == -1);
    assert(trierarch_dmabuf_feedback_prepare("", false) == -1);
    assert(trierarch_dmabuf_feedback_prepare(NULL, true) == -1);
    assert(trierarch_dmabuf_feedback_prepare("", true) == -1);

    char path[4096];
    int length = snprintf(path, sizeof(path), "%s/dmabuf-feedback.sock", runtime_dir);
    assert(length > 0 && (size_t)length < sizeof(path));
    unlink(path);
    rmdir(runtime_dir);
    puts("dmabuf feedback path tests passed");
    return 0;
}
