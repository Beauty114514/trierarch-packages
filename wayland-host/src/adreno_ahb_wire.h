#ifndef TRIERARCH_ADRENO_AHB_WIRE_H
#define TRIERARCH_ADRENO_AHB_WIRE_H

#include "adreno_ahb_native.h"

#include <stdbool.h>

/* Rebuild an AHardwareBuffer from unchanged handle metadata plus two FDs. */
bool trierarch_adreno_ahb_register_handle(const AHardwareBuffer_Desc *description,
        const struct trierarch_adreno_native_handle *handle, int pixel_fd,
        int metadata_fd, AHardwareBuffer **received);

#endif
