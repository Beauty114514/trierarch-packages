#include "dmabuf_ahb.h"

#include "adreno_ahb_import.h"
#include "adreno_ahb_padded.h"
#include "server_internal.h"

#include <android/hardware_buffer.h>
#include <android/log.h>

#define TAG "TrierarchAdrenoAhb"
#define DRM_FORMAT_XRGB8888 0x34325258u

void *trierarch_dmabuf_ahb_get(struct shm_buffer *buffer, int frame_fd) {
    if (!buffer || !buffer->dmabuf || frame_fd < 0)
        return NULL;
    if (buffer->dmabuf_ahb_attempted)
        return buffer->dmabuf_hardware_buffer;
    buffer->dmabuf_ahb_attempted = true;

    /* Only the one-plane, offset-zero, explicitly linear XRGB layout was
     * verified against the guest allocator. Never reinterpret other layouts. */
    if (buffer->format != DRM_FORMAT_XRGB8888 || buffer->dmabuf_modifier != 0 ||
            buffer->dmabuf_offset != 0 || buffer->width <= 0 || buffer->width > 4096 ||
            buffer->height <= 0 || buffer->height > 4096 ||
            buffer->stride < (int64_t)buffer->width * 4)
        return NULL;

    static bool preflight_checked;
    static bool preflight_usable;
    static uint32_t donor_format;
    if (!preflight_checked) {
        struct trierarch_adreno_ahb_preflight preflight = {0};
        preflight_usable = trierarch_adreno_ahb_preflight_run(buffer->width,
                buffer->height, buffer->format, &preflight) && preflight.candidate;
        donor_format = preflight.donor_format;
        preflight_checked = true;
        __android_log_print(ANDROID_LOG_INFO, TAG,
                "normal dma-buf AHB preflight: usable=%d", preflight_usable);
    }
    if (!preflight_usable)
        return NULL;

    AHardwareBuffer *imported = NULL;
    uint32_t allocation_height = 0;
    if (!trierarch_adreno_ahb_linear_import(frame_fd, (uint32_t)buffer->width,
                (uint32_t)buffer->height, (uint32_t)buffer->stride, donor_format,
                &imported, &allocation_height)) {
        __android_log_print(ANDROID_LOG_INFO, TAG,
                "normal dma-buf AHB import rejected: %dx%d stride=%d; EGL/CPU fallback",
                buffer->width, buffer->height, buffer->stride);
        return NULL;
    }
    buffer->dmabuf_hardware_buffer = imported;
    buffer->dmabuf_allocation_height = allocation_height;
    __android_log_print(ANDROID_LOG_INFO, TAG,
            "normal dma-buf AHB import ready: %dx%d allocation-height=%u stride=%d",
            buffer->width, buffer->height, allocation_height, buffer->stride);
    return imported;
}
