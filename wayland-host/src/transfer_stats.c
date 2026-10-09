#include "transfer_stats.h"

#include <android/log.h>
#include <string.h>

#define REPORT_INTERVAL_NS 5000000000ULL
#define TAG "TrierarchRenderer"

static const char *const path_names[TRIERARCH_TRANSFER_PATH_COUNT] = {
    "host-ahb", "egl-wayland", "egl-dmabuf", "adreno-copy", "cpu-upload", "dropped",
};

void trierarch_transfer_stats_record(struct trierarch_transfer_stats *stats,
        enum trierarch_transfer_path path, size_t logical_bytes, uint64_t cpu_ns) {
    if (!stats || (unsigned int)path >= TRIERARCH_TRANSFER_PATH_COUNT) return;
    struct trierarch_transfer_path_stats *entry = &stats->paths[path];
    entry->draws++;
    entry->logical_bytes += logical_bytes;
    entry->cpu_ns += cpu_ns;
    if (cpu_ns > entry->cpu_max_ns) entry->cpu_max_ns = cpu_ns;
}

void trierarch_transfer_stats_report(struct trierarch_transfer_stats *stats,
        uint64_t now_ns) {
    if (!stats) return;
    if (!stats->last_report_ns) {
        stats->last_report_ns = now_ns;
        return;
    }
    uint64_t interval_ns = now_ns - stats->last_report_ns;
    if (interval_ns < REPORT_INTERVAL_NS) return;
    for (unsigned int path = 0; path < TRIERARCH_TRANSFER_PATH_COUNT; ++path) {
        struct trierarch_transfer_path_stats *entry = &stats->paths[path];
        if (!entry->draws) continue;
        __android_log_print(ANDROID_LOG_INFO, TAG,
                "transfer path=%s interval=%.1fs buffer-draws=%llu logical-mib=%.1f "
                "cpu-submit-us avg/max=%llu/%llu",
                path_names[path], (double)interval_ns / 1000000000.0,
                (unsigned long long)entry->draws,
                (double)entry->logical_bytes / 1048576.0,
                (unsigned long long)(entry->cpu_ns / entry->draws / 1000ULL),
                (unsigned long long)(entry->cpu_max_ns / 1000ULL));
    }
    memset(stats->paths, 0, sizeof(stats->paths));
    stats->last_report_ns = now_ns;
}
