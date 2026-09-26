#define _POSIX_C_SOURCE 200809L

#include <gbm.h>
#include <xf86drm.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

enum { PROBE_WIDTH = 64, PROBE_HEIGHT = 64 };

int main(void) {
    int render_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (render_fd < 0) {
        perror("open /dev/dri/renderD128");
        return EXIT_FAILURE;
    }
    drmVersionPtr version = drmGetVersion(render_fd);
    if (version) {
        printf("DRM driver: %s %d.%d.%d\n", version->name ? version->name : "unknown",
                version->version_major, version->version_minor, version->version_patchlevel);
        drmFreeVersion(version);
    } else {
        fprintf(stderr, "drmGetVersion failed; continuing with GBM.\n");
    }
    struct gbm_device *device = gbm_create_device(render_fd);
    if (!device) {
        fprintf(stderr, "gbm_create_device failed.\n");
        close(render_fd);
        return EXIT_FAILURE;
    }
    printf("GBM backend: %s\n", gbm_device_get_backend_name(device));
    uint32_t usage = GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR;
    struct gbm_bo *buffer = gbm_bo_create(device, PROBE_WIDTH, PROBE_HEIGHT,
            GBM_FORMAT_XRGB8888, usage);
    if (!buffer) {
        fprintf(stderr, "linear GBM allocation unavailable; retrying without GBM_BO_USE_LINEAR.\n");
        usage = GBM_BO_USE_RENDERING;
        buffer = gbm_bo_create(device, PROBE_WIDTH, PROBE_HEIGHT,
                GBM_FORMAT_XRGB8888, usage);
        if (!buffer) {
            fprintf(stderr, "gbm_bo_create failed for an XRGB8888 render buffer.\n");
            gbm_device_destroy(device);
            close(render_fd);
            return EXIT_FAILURE;
        }
    }
    int dmabuf_fd = gbm_bo_get_fd(buffer);
    if (dmabuf_fd < 0) {
        fprintf(stderr, "gbm_bo_get_fd failed.\n");
        gbm_bo_destroy(buffer);
        gbm_device_destroy(device);
        close(render_fd);
        return EXIT_FAILURE;
    }
    struct stat statbuf;
    if (fstat(dmabuf_fd, &statbuf) != 0) {
        perror("fstat exported dma-buf");
        close(dmabuf_fd);
        gbm_bo_destroy(buffer);
        gbm_device_destroy(device);
        close(render_fd);
        return EXIT_FAILURE;
    }
    printf("dma-buf exported: fd=%d %ux%u stride=%u inode=%llu usage=0x%x\n",
            dmabuf_fd, gbm_bo_get_width(buffer), gbm_bo_get_height(buffer),
            gbm_bo_get_stride(buffer), (unsigned long long)statbuf.st_ino, usage);
    close(dmabuf_fd);
    gbm_bo_destroy(buffer);
    gbm_device_destroy(device);
    close(render_fd);
    return EXIT_SUCCESS;
}
