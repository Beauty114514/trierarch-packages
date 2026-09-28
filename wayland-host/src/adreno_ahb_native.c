#include "adreno_ahb_native.h"

#include <dlfcn.h>
#include <stdbool.h>

typedef const struct trierarch_adreno_native_handle *(*get_native_handle_fn)(
        const AHardwareBuffer *);

static get_native_handle_fn native_handle_function(void) {
    static bool attempted;
    static get_native_handle_fn function;
    if (!attempted) {
        attempted = true;
        function = (get_native_handle_fn)dlsym(RTLD_DEFAULT,
                "AHardwareBuffer_getNativeHandle");
        if (!function) {
            void *library = dlopen("libnativewindow.so", RTLD_NOW | RTLD_LOCAL);
            if (library)
                function = (get_native_handle_fn)dlsym(library,
                        "AHardwareBuffer_getNativeHandle");
        }
    }
    return function;
}

const struct trierarch_adreno_native_handle *
trierarch_adreno_ahb_native_handle(const AHardwareBuffer *buffer) {
    get_native_handle_fn function = native_handle_function();
    return function ? function(buffer) : NULL;
}

bool trierarch_adreno_ahb_native_handle_available(void) {
    return native_handle_function() != NULL;
}
