#ifndef TRIERARCH_DMABUF_PRESENTATION_H
#define TRIERARCH_DMABUF_PRESENTATION_H

#include <stdbool.h>

struct compositor_surface;
struct shm_buffer;
struct wayland_server;

/* Submit the surface's newly committed dma-buf to its mailbox queue. */
bool trierarch_dmabuf_surface_submit(struct compositor_surface *surface,
        struct shm_buffer *buffer);

/* Promote each surface's newest queued frame before composition. */
void trierarch_dmabuf_surface_latch_frames(struct wayland_server *server);

/* Retire active and queued presentation leases without freeing the queue. */
void trierarch_dmabuf_surface_retire(struct compositor_surface *surface);

/* Retire all leases and free a surface's queue during surface teardown. */
void trierarch_dmabuf_surface_destroy(struct compositor_surface *surface);

#endif
