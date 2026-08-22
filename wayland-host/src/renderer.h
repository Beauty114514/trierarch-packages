#ifndef TRIERARCH_WAYLAND_RENDERER_H
#define TRIERARCH_WAYLAND_RENDERER_H

#include <stdbool.h>
#include <android/native_window.h>

struct wayland_server;
struct renderer_context;

struct renderer_context *trierarch_renderer_create(ANativeWindow *window,
        struct wayland_server *server);
void trierarch_renderer_destroy(struct renderer_context *renderer);
bool trierarch_renderer_valid(const struct renderer_context *renderer);
bool trierarch_renderer_render(struct renderer_context *renderer,
        struct wayland_server *server);

#endif
