#include "compositor.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int exists(const char *runtime_dir, const char *name) {
    char path[4096];
    int length = snprintf(path, sizeof(path), "%s/%s", runtime_dir, name);
    assert(length > 0 && (size_t)length < sizeof(path));
    return access(path, F_OK) == 0;
}

int main(void) {
    char template[] = "/tmp/trierarch-wayland-server-XXXXXX";
    char *runtime_dir = mkdtemp(template);
    assert(runtime_dir != NULL);

    wayland_server_t *server = trierarch_wayland_create(runtime_dir, false);
    assert(server != NULL);
    assert(exists(runtime_dir, "wayland-trierarch"));
    assert(!exists(runtime_dir, "dmabuf-feedback.sock"));

    trierarch_wayland_destroy(server);
    assert(!exists(runtime_dir, "wayland-trierarch"));
    assert(!exists(runtime_dir, "dmabuf-feedback.sock"));
    rmdir(runtime_dir);

    puts("wayland server create/destroy test passed");
    return 0;
}
