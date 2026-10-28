#define _GNU_SOURCE
#include <gbm.h>

#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv) {
    const char *node = argc > 1 ? argv[1] : "/dev/dri/renderD128";
    int drm_fd = open(node, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        perror(node);
        return 2;
    }

    struct gbm_device *device = gbm_create_device(drm_fd);
    if (!device) {
        fprintf(stderr, "gbm_create_device failed\n");
        return 2;
    }

    struct gbm_bo *bo = gbm_bo_create(device, 256, 256, GBM_FORMAT_ABGR8888,
                                      GBM_BO_USE_RENDERING);
    if (!bo) {
        fprintf(stderr, "gbm_bo_create failed\n");
        return 3;
    }

    int token_fd = gbm_bo_get_fd(bo);
    if (token_fd < 0) {
        fprintf(stderr, "gbm_bo_get_fd failed\n");
        return 4;
    }

    struct stat st;
    if (fstat(token_fd, &st) != 0) {
        perror("fstat(token)");
        return 4;
    }

    printf("token ino=%llu stride=%u format=0x%x width=%u height=%u\n",
           (unsigned long long)st.st_ino,
           gbm_bo_get_stride(bo),
           gbm_bo_get_format(bo),
           gbm_bo_get_width(bo),
           gbm_bo_get_height(bo));
    printf("PASS\n");
    return 0;
}
