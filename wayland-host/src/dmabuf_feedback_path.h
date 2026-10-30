#ifndef TRIERARCH_WAYLAND_DMABUF_FEEDBACK_PATH_H
#define TRIERARCH_WAYLAND_DMABUF_FEEDBACK_PATH_H

#include <stdbool.h>

/*
 * Clears any feedback socket left by an earlier dmabuf session.
 * Returns 0 when the runtime directory is valid, -1 otherwise.
 */
int trierarch_clear_dmabuf_feedback_socket(const char *runtime_dir);

/*
 * Prepares the feedback socket for a new server. A shm session must not
 * create the socket; any stale one is removed. A dmabuf session leaves the
 * path for trierarch_dmabuf_feedback_device_create().
 */
int trierarch_dmabuf_feedback_prepare(const char *runtime_dir, bool enabled);

#endif
