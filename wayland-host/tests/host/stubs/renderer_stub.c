#include "renderer.h"
#include "server_internal.h"
struct renderer_context *trierarch_renderer_create(ANativeWindow *window, struct wayland_server *server) {
    (void)window; (void)server; return NULL;
}
void trierarch_renderer_destroy(struct renderer_context *renderer) { (void)renderer; }
bool trierarch_renderer_valid(const struct renderer_context *renderer) { (void)renderer; return false; }
bool trierarch_renderer_render(struct renderer_context *renderer, struct wayland_server *server) {
    (void)renderer; (void)server; return false;
}
void trierarch_renderer_report_performance(struct wayland_server *server) { (void)server; }
void trierarch_renderer_wait_idle(struct renderer_context *renderer) { (void)renderer; }
