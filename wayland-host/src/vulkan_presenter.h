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

/* Configure a one-shot import check for a real Wayland dma-buf. */
void trierarch_vulkan_configure_dmabuf_probe(const char *adrenotools_library,
        const char *driver_dir, const char *hook_dir, const char *tmp_dir);
bool trierarch_vulkan_dmabuf_probe_configured(void);
int trierarch_vulkan_probe_dmabuf(int fd, int width, int height, int stride,
        unsigned int format, unsigned long long modifier);

typedef struct trierarch_vulkan_presenter trierarch_vulkan_presenter;
bool trierarch_vulkan_presenter_configured(void);
trierarch_vulkan_presenter *trierarch_vulkan_presenter_create(ANativeWindow *window);
struct trierarch_vulkan_draw {
    int fd;
    int width;
    int height;
    int stride;
    unsigned int format;
    unsigned long long modifier;
    int dst_x;
    int dst_y;
    int dst_width;
    int dst_height;
};

int trierarch_vulkan_presenter_present(trierarch_vulkan_presenter *presenter,
        int fd, int width, int height, int stride, unsigned int format,
        unsigned long long modifier);
int trierarch_vulkan_presenter_present_scene(trierarch_vulkan_presenter *presenter,
        const struct trierarch_vulkan_draw *root,
        const struct trierarch_vulkan_draw *cursor);
void trierarch_vulkan_presenter_set_cursor(trierarch_vulkan_presenter *presenter,
        const struct trierarch_vulkan_draw *cursor);
void trierarch_vulkan_presenter_wait_idle(trierarch_vulkan_presenter *presenter);
void trierarch_vulkan_presenter_destroy(trierarch_vulkan_presenter *presenter);

#endif
