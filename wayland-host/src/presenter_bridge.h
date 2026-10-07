#ifndef TRIERARCH_PRESENTER_BRIDGE_H
#define TRIERARCH_PRESENTER_BRIDGE_H

struct wayland_server;
struct trierarch_presenter_bridge;

struct trierarch_presenter_bridge *trierarch_presenter_bridge_create(
        struct wayland_server *server, const char *runtime_dir);
void trierarch_presenter_bridge_destroy(
        struct trierarch_presenter_bridge *bridge);

#endif
