#include "gpu_probe.h"
#include "adreno_dmabuf_import.h"

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <stdbool.h>
#include <stdlib.h>

#define TAG "TrierarchGpuProbe"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

struct trierarch_gpu_probe {
    struct trierarch_adreno_importer *adreno_importer;
};

struct trierarch_gpu_probe *trierarch_gpu_probe_create(struct wayland_server *server,
        const char *runtime_dir) {
    (void)server;
    (void)runtime_dir;
    return calloc(1, sizeof(struct trierarch_gpu_probe));
}

void trierarch_gpu_probe_destroy(struct trierarch_gpu_probe *probe) {
    if (!probe) return;
    trierarch_adreno_importer_destroy(probe->adreno_importer);
    free(probe);
}

bool trierarch_gpu_probe_set_adreno_driver(struct trierarch_gpu_probe *probe,
        const char *hook_library_dir, const char *driver_dir, const char *driver_name) {
    if (!probe) return false;
    char error[160] = {0};
    struct trierarch_adreno_importer *importer = trierarch_adreno_importer_create(
            hook_library_dir, driver_dir, driver_name, error, sizeof(error));
    if (!importer) {
        LOGE("Adreno dma-buf importer setup failed: %s", error);
        return false;
    }
    trierarch_adreno_importer_destroy(probe->adreno_importer);
    probe->adreno_importer = importer;
    LOGI("Adreno dma-buf importer configured");
    return true;
}

AHardwareBuffer *trierarch_gpu_probe_copy_adreno(struct trierarch_gpu_probe *probe,
        const struct trierarch_gpu_probe_buffer *buffer, int buffer_fd) {
    if (!probe || !probe->adreno_importer || !buffer || buffer_fd < 0) return NULL;
    const struct trierarch_dmabuf_descriptor descriptor = {
        .fd = buffer_fd, .width = buffer->width, .height = buffer->height,
        .format = buffer->drm_format, .stride = buffer->stride, .offset = 0,
        .modifier = buffer->modifier,
    };
    char error[160] = {0};
    struct trierarch_adreno_imported_image *image = trierarch_adreno_importer_import(
            probe->adreno_importer, &descriptor, error, sizeof(error));
    if (!image) {
        static unsigned import_failures;
        if (import_failures++ < 3)
            LOGE("Adreno dma-buf import rejected before copy: %s", error);
        return NULL;
    }
    AHardwareBuffer *output = trierarch_adreno_imported_image_copy_to_ahardware_buffer(
            image, error, sizeof(error));
    trierarch_adreno_imported_image_destroy(image);
    if (!output) {
        static unsigned copy_failures;
        if (copy_failures++ < 3)
            LOGE("Adreno dma-buf copy rejected: %s", error);
    } else {
        static unsigned copy_successes;
        if (copy_successes++ < 3)
            LOGI("Adreno dma-buf copy accepted: %ux%u", buffer->width, buffer->height);
    }
    return output;
}
