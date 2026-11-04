#ifndef TRIERARCH_VULKAN_PRESENTER_H
#define TRIERARCH_VULKAN_PRESENTER_H

#include <android/native_window.h>
#include <stdbool.h>

/*
 * Development-only Vulkan presenter self-test.  It loads an external
 * AdrenoTools Turnip driver, creates an Android Vulkan surface, clears and
 * presents a fixed number of frames, then tears everything down.
 *
 * Returns 0 on success.  The production Wayland renderer will grow out of
 * this module once dmabuf import is wired in.
 */
int trierarch_vulkan_presenter_self_test(ANativeWindow *window,
        const char *adrenotools_library, const char *driver_dir,
        const char *hook_dir, const char *tmp_dir);

#endif
