#include "adreno_ahb_import.h"
#include "adreno_ahb_roundtrip.h"

#include <android/hardware_buffer.h>
#include <android/log.h>

#include <string.h>

#define TAG "TrierarchAdrenoAhb"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

#define DRM_FORMAT_ABGR8888 0x34324241u
#define DRM_FORMAT_XRGB8888 0x34325258u

bool trierarch_adreno_ahb_preflight_run(uint32_t width, uint32_t height,
        uint32_t drm_format, struct trierarch_adreno_ahb_preflight *result) {
    if (!result || !width || !height)
        return false;
    memset(result, 0, sizeof(*result));

    /* The probe clients use one-plane 32-bit RGB buffers. Pixel conversion
     * is not part of the read-only allocator-layout calibration. */
    if (drm_format != DRM_FORMAT_ABGR8888 && drm_format != DRM_FORMAT_XRGB8888) {
        LOGI("preflight skipped for unsupported DRM format=0x%x", drm_format);
        return false;
    }

    if (!trierarch_adreno_ahb_layout_calibrate(
                AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, &result->layout))
        return false;

    result->donor_format = result->layout.donor_format;
    result->first_stride = result->layout.first_stride;
    result->second_stride = result->layout.second_stride;
    result->native_handle_fds = result->layout.native_handle_fds;
    result->native_handle_ints = result->layout.native_handle_ints;
    result->metadata_bytes = result->layout.metadata_bytes;
    result->donor_roundtrip = result->layout.candidate &&
            trierarch_adreno_ahb_roundtrip_run(result->donor_format);
    result->candidate = result->layout.candidate && result->donor_roundtrip;
    LOGI("AHB donor preflight: guest=%ux%u format=0x%x donors strides=%u/%u "
            "fds=%d ints=%d metadata=%llu roundtrip=%d candidate=%d",
            width, height, drm_format, result->first_stride, result->second_stride,
            result->native_handle_fds, result->native_handle_ints,
            (unsigned long long)result->metadata_bytes, result->donor_roundtrip,
            result->candidate);
    return true;
}
