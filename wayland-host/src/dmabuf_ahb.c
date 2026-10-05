#include "dmabuf_ahb.h"

#include "adreno_ahb_donor.h"
#include "adreno_ahb_import.h"
#include "adreno_ahb_padded.h"
#include "server_internal.h"

#include <android/hardware_buffer.h>
#include <android/log.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TAG "TrierarchAdrenoAhb"
#define DRM_FORMAT_XRGB8888 0x34325258u
#define DRM_FORMAT_MOD_INVALID 0x00ffffffffffffffULL

static const char *import_gate_reason(const struct shm_buffer *buffer, int frame_fd) {
    if (!buffer || !buffer->dmabuf)
        return "not-a-dmabuf";
    if (frame_fd < 0)
        return "no-frame-fd";
    if (buffer->format != DRM_FORMAT_XRGB8888)
        return "unsupported-format";
    if (buffer->dmabuf_modifier == DRM_FORMAT_MOD_INVALID)
        return "implicit-modifier-needs-calibration";
    if (buffer->dmabuf_modifier != 0)
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

static bool donor_probe_enabled(void) {
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (!runtime_dir || !*runtime_dir)
        return false;
    char path[PATH_MAX];
    int length = snprintf(path, sizeof(path), "%s/adreno-ahb-donor-probe.enable", runtime_dir);
    if (length < 0 || (size_t)length >= sizeof(path))
        return false;
    struct stat status = {0};
    return stat(path, &status) == 0 && S_ISREG(status.st_mode) &&
            status.st_uid == getuid();
}

static bool preflight_for_xrgb(const struct shm_buffer *buffer, uint32_t *donor_format) {
    static bool checked;
    static bool usable;
    static uint32_t format;
    if (!checked) {
        struct trierarch_adreno_ahb_preflight preflight = {0};
        usable = trierarch_adreno_ahb_preflight_run(buffer->width, buffer->height,
                buffer->format, &preflight) && preflight.candidate;
        format = preflight.donor_format;
        checked = true;
        __android_log_print(ANDROID_LOG_INFO, TAG,
                "normal dma-buf AHB preflight: usable=%d", usable);
    }
    if (usable && donor_format)
        *donor_format = format;
    return usable;
}

static void probe_implicit_donor(const struct shm_buffer *buffer, int frame_fd) {
    if (!donor_probe_enabled())
        return;
    uint32_t donor_format = 0;
    if (!preflight_for_xrgb(buffer, &donor_format)) {
        __android_log_print(ANDROID_LOG_INFO, TAG,
                "implicit donor comparison skipped: allocator preflight unavailable");
        return;
    }
    struct trierarch_adreno_ahb_donor_match result = {0};
    if (!trierarch_adreno_ahb_compare_donor(frame_fd, (uint32_t)buffer->width,
                (uint32_t)buffer->height, (uint32_t)buffer->stride,
                donor_format, &result)) {
        __android_log_print(ANDROID_LOG_INFO, TAG,
                "implicit donor comparison failed to allocate or inspect donor");
    }
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
                "normal dma-buf AHB skipped: reason=%s fmt=0x%x mod=0x%llx "
                "offset=%u size=%dx%d stride=%d",
                reason, buffer->format, (unsigned long long)buffer->dmabuf_modifier,
                buffer->dmabuf_offset, buffer->width, buffer->height, buffer->stride);
        if (strcmp(reason, "implicit-modifier-needs-calibration") == 0)
            probe_implicit_donor(buffer, frame_fd);
        return NULL;
    }

    static uint32_t donor_format;
    if (!preflight_for_xrgb(buffer, &donor_format))
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
