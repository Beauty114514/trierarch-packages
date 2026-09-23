#ifndef TRIERARCH_GPU_PROBE_HOST_H
#define TRIERARCH_GPU_PROBE_HOST_H

#include <android/native_window.h>

/* Runs synchronously and accepts one guest buffer.  This is intentionally a
 * test-only API; production code will need a non-blocking queue and fences. */
int trierarch_gpu_probe_accept_one(ANativeWindow *window, const char *socket_path);

#endif
