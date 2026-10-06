#include "dmabuf_ahb.h"

#include "adreno_ahb_guest.h"
#include "adreno_ahb_import.h"
#include "adreno_ahb_padded.h"
#include "server_internal.h"

#include <android/hardware_buffer.h>
#include <android/log.h>

#include <sys/stat.h>
#include <unistd.h>

#define TAG "TrierarchDmaBuf"
#define DRM_FORMAT_XRGB8888 0x34325258u
#define DRM_FORMAT_ARGB8888 0x34325241u
#define DRM_FORMAT_MOD_INVALID 0x00ffffffffffffffULL

static const char *import_gate_reason(const struct shm_buffer *buffer, int frame_fd) {
    if (!buffer || !buffer->dmabuf)
        return "not-a-dmabuf";
    if (frame_fd < 0)
        return "no-frame-fd";
    /* AR24/XR24 are the single-plane BGRA memory layouts emitted by the
     * KGSL Mesa path.  Other fourcc layouts still use the CPU fallback until
     * they have an equally strict Android donor mapping. */
    if (buffer->format != DRM_FORMAT_XRGB8888 && buffer->format != DRM_FORMAT_ARGB8888)
        return "unsupported-format";
    /* INVALID is the protocol spelling for an implicit layout.  This path
     * does not infer that layout: registration remains conditional on an
     * Android donor whose measured geometry and allocation exactly match. */
    if (buffer->dmabuf_modifier != DRM_FORMAT_MOD_INVALID && buffer->dmabuf_modifier != 0)
        return "non-linear-modifier";
    if (buffer->dmabuf_offset != 0)
        return "non-zero-offset";
    if (buffer->width <= 0 || buffer->width > 4096 ||
            buffer->height <= 0 || buffer->height > 4096)
        return "unsupported-dimensions";
    if (buffer->stride < (int64_t)buffer->width * 4)
        return "short-stride";
    return NULL;
}

static bool preflight_for_bgra(const struct shm_buffer *buffer,
        struct trierarch_adreno_ahb_preflight *result) {
    static bool checked;
    static bool usable;
    static struct trierarch_adreno_ahb_preflight cached;
    if (!checked) {
        usable = trierarch_adreno_ahb_preflight_run(buffer->width, buffer->height,
                buffer->format, &cached) && cached.candidate;
        checked = true;
        __android_log_print(ANDROID_LOG_INFO, TAG,
                "dmabuf import stage=ahb-preflight result=%s",
                usable ? "ready" : "unavailable");
    }
    if (usable && result)
        *result = cached;
    return usable;
}

AHardwareBuffer *trierarch_dmabuf_ahb_get(struct shm_buffer *buffer, int frame_fd) {
    if (!buffer || !buffer->dmabuf || frame_fd < 0)
        return NULL;
    if (buffer->dmabuf_ahb_attempted)
        return buffer->dmabuf_hardware_buffer;
    buffer->dmabuf_ahb_attempted = true;

    const char *reason = import_gate_reason(buffer, frame_fd);
    if (reason) {
        __android_log_print(ANDROID_LOG_INFO, TAG,
                "dmabuf import stage=eligibility result=skipped reason=%s fmt=0x%x mod=0x%llx "
                "offset=%u size=%dx%d stride=%d",
                reason, buffer->format, (unsigned long long)buffer->dmabuf_modifier,
                buffer->dmabuf_offset, buffer->width, buffer->height, buffer->stride);
        return NULL;
    }

    struct trierarch_adreno_ahb_preflight preflight = {0};
    if (!preflight_for_bgra(buffer, &preflight))
        return NULL;

    AHardwareBuffer *imported = NULL;
    uint32_t allocation_height = 0;
    const char *path = "matching-donor";
    if (!trierarch_adreno_ahb_linear_import(frame_fd, (uint32_t)buffer->width,
                (uint32_t)buffer->height, (uint32_t)buffer->stride, preflight.donor_format,
                &imported, &allocation_height)) {
        path = "patched-donor";
        allocation_height = (uint32_t)buffer->height;
        if (!trierarch_adreno_ahb_guest_fd_import(&preflight.layout, frame_fd,
                    (uint32_t)buffer->width, (uint32_t)buffer->height,
                    (uint32_t)buffer->stride, preflight.donor_format, &imported)) {
            __android_log_print(ANDROID_LOG_INFO, TAG,
                    "dmabuf import stage=ahb-register result=rejected size=%dx%d stride=%d",
                    buffer->width, buffer->height, buffer->stride);
            return NULL;
        }
    }
    buffer->dmabuf_hardware_buffer = imported;
    buffer->dmabuf_allocation_height = allocation_height;
    __android_log_print(ANDROID_LOG_INFO, TAG,
            "dmabuf import stage=ahb-register result=ready path=%s size=%dx%d "
            "allocation-height=%u stride=%d",
            path, buffer->width, buffer->height, allocation_height, buffer->stride);
    return imported;
}
