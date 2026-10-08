#ifndef TRIERARCH_DMABUF_DEVICE_REPORT_PROTOCOL_H
#define TRIERARCH_DMABUF_DEVICE_REPORT_PROTOCOL_H

#include <stdint.h>

#define TRIERARCH_DMABUF_DEVICE_REPORT_MAGIC 0x54444642u /* TDFB */
#define TRIERARCH_DMABUF_DEVICE_REPORT_VERSION 1u

/*
 * Both endpoints are local arm64 processes joined by the runtime bind mount.
 * Keep this fixed-width message deliberately small: it reports device identity
 * only; it does not transfer dma-buf file descriptors or rendering state.
 */
struct trierarch_dmabuf_device_report {
    uint32_t magic;
    uint32_t version;
    uint32_t major;
    uint32_t minor;
};

#endif
