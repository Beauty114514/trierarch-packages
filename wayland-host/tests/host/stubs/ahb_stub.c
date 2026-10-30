#include <android/hardware_buffer.h>
#include <stdlib.h>
int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *desc, AHardwareBuffer **out) {
    (void)desc;
    if (!out) return -1;
    *out = NULL;
    return -1;
}
void AHardwareBuffer_acquire(AHardwareBuffer *buffer) { (void)buffer; }
void AHardwareBuffer_release(AHardwareBuffer *buffer) { (void)buffer; }
