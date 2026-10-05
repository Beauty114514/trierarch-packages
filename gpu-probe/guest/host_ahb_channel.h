#ifndef TRIERARCH_GPU_PROBE_HOST_AHB_CHANNEL_H
#define TRIERARCH_GPU_PROBE_HOST_AHB_CHANNEL_H

#include "../protocol.h"

int trierarch_host_ahb_connect(const char *path);
int trierarch_host_ahb_request_buffers(int fd);
int trierarch_host_ahb_receive_buffer(int fd,
        struct trierarch_gpu_probe_buffer *buffer);

#endif
