#ifndef TRIERARCH_WAYLAND_COMPOSITOR_H
#define TRIERARCH_WAYLAND_COMPOSITOR_H

#include <stdbool.h>
#include <stdint.h>

typedef struct wayland_server wayland_server_t;

wayland_server_t *trierarch_wayland_create(const char *runtime_dir);
void trierarch_wayland_destroy(wayland_server_t *server);
void trierarch_wayland_dispatch(wayland_server_t *server);
void trierarch_wayland_set_output_size(wayland_server_t *server, int width, int height);
bool trierarch_wayland_has_surface(wayland_server_t *server);

#endif
