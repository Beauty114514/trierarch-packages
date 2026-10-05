#ifndef TRIERARCH_DMABUF_IDENTITY_H
#define TRIERARCH_DMABUF_IDENTITY_H

/* Read-only diagnostics for a dma-buf already owned by the host process. */
void trierarch_dmabuf_log_process_identity(void);
void trierarch_dmabuf_log_identity(const char *label, int fd);

#endif
