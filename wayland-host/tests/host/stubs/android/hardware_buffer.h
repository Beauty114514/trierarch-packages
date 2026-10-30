#ifndef TRIERARCH_TEST_AHARDWARE_BUFFER_H
#define TRIERARCH_TEST_AHARDWARE_BUFFER_H
#include <stdint.h>
typedef struct AHardwareBuffer AHardwareBuffer;
typedef struct AHardwareBuffer_Desc {
    uint32_t width;
    uint32_t height;
    uint32_t layers;
    uint32_t format;
    uint64_t usage;
    uint32_t stride;
    uint32_t rfu0;
    uint64_t rfu1;
} AHardwareBuffer_Desc;
#define AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM 1
#define AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE 0x100ULL
#define AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER 0x200ULL
#define AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN 0x400ULL
int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *, AHardwareBuffer **);
void AHardwareBuffer_acquire(AHardwareBuffer *);
void AHardwareBuffer_release(AHardwareBuffer *);
#endif
