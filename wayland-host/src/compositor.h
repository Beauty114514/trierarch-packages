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
void trierarch_pointer_move_absolute(wayland_server_t *server,
        float x, float y, uint32_t time_ms);
void trierarch_pointer_move_relative(wayland_server_t *server,
        float delta_x, float delta_y, uint32_t time_ms);
void trierarch_pointer_set_button(wayland_server_t *server,
        int button, bool pressed, uint32_t time_ms);
void trierarch_pointer_scroll(wayland_server_t *server,
        float delta_x, float delta_y, uint32_t source, uint32_t time_ms);
void trierarch_pointer_reset(wayland_server_t *server, uint32_t time_ms);
void trierarch_pointer_set_cursor_visible(wayland_server_t *server, bool visible);

#endif
