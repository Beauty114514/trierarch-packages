#ifndef TRIERARCH_EXPLICIT_SYNC_H
#define TRIERARCH_EXPLICIT_SYNC_H

#include <stdbool.h>

struct wl_resource;

/* Protocol state is deliberately prepared before its global is advertised.
 * A client must not observe the explicit-sync global until renderer-side
 * acquire waiting and release delivery are both implemented. */
struct trierarch_explicit_sync_state {
    int pending_acquire_fence_fd;
    int current_acquire_fence_fd;
    struct wl_resource *pending_release;
    struct wl_resource *current_release;
};

void trierarch_explicit_sync_state_init(struct trierarch_explicit_sync_state *state);
void trierarch_explicit_sync_state_commit(struct trierarch_explicit_sync_state *state,
        bool attached);
void trierarch_explicit_sync_state_destroy(struct trierarch_explicit_sync_state *state);

#endif
