#ifndef TRIERARCH_DMABUF_AHB_H
#define TRIERARCH_DMABUF_AHB_H

struct shm_buffer;

/* Returns a buffer-owned handle, or NULL so the renderer can use EGL/CPU. */
void *trierarch_dmabuf_ahb_get(struct shm_buffer *buffer, int frame_fd);

#endif
