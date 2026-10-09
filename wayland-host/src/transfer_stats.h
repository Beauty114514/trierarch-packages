#ifndef TRIERARCH_TRANSFER_STATS_H
#define TRIERARCH_TRANSFER_STATS_H

#include <stddef.h>
#include <stdint.h>

enum trierarch_transfer_path {
    TRIERARCH_TRANSFER_HOST_AHB,
    TRIERARCH_TRANSFER_EGL_WAYLAND,
    TRIERARCH_TRANSFER_EGL_DMABUF,
    TRIERARCH_TRANSFER_ADRENO_COPY,
    TRIERARCH_TRANSFER_CPU_UPLOAD,
    TRIERARCH_TRANSFER_DROPPED,
    TRIERARCH_TRANSFER_PATH_COUNT,
};

struct trierarch_transfer_path_stats {
    /* Counts buffer draws, not distinct buffers. logical_bytes is the
     * buffer extent, not a measurement of bytes copied across devices. */
    uint64_t draws;
    uint64_t logical_bytes;
    uint64_t cpu_ns;
    uint64_t cpu_max_ns;
};

struct trierarch_transfer_stats {
    uint64_t last_report_ns;
    struct trierarch_transfer_path_stats paths[TRIERARCH_TRANSFER_PATH_COUNT];
};

void trierarch_transfer_stats_record(struct trierarch_transfer_stats *stats,
        enum trierarch_transfer_path path, size_t logical_bytes, uint64_t cpu_ns);
void trierarch_transfer_stats_report(struct trierarch_transfer_stats *stats,
        uint64_t now_ns);

#endif
