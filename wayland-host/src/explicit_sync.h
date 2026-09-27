#ifndef TRIERARCH_EXPLICIT_SYNC_H
#define TRIERARCH_EXPLICIT_SYNC_H

#include <stdbool.h>
#include <stdint.h>

struct wl_client;
struct wl_resource;
struct trierarch_explicit_release;

/* Protocol state is deliberately prepared before its global is advertised.
 * A client must not observe the explicit-sync global until renderer-side
 * acquire waiting and release delivery are both implemented. */
struct trierarch_explicit_sync_state {
    int pending_acquire_fence_fd;
    int current_acquire_fence_fd;
    struct trierarch_explicit_release *pending_release;
    struct trierarch_explicit_release *current_release;
    struct wl_resource *resource;
};

void trierarch_explicit_sync_state_init(struct trierarch_explicit_sync_state *state);
void trierarch_explicit_sync_state_commit(struct trierarch_explicit_sync_state *state,
        bool attached);
bool trierarch_explicit_sync_state_has_pending(
        const struct trierarch_explicit_sync_state *state);
void trierarch_explicit_sync_state_discard_pending(
        struct trierarch_explicit_sync_state *state);
int trierarch_explicit_sync_state_take_acquire_fence(
        struct trierarch_explicit_sync_state *state);
struct trierarch_explicit_release *trierarch_explicit_sync_state_take_release(
        struct trierarch_explicit_sync_state *state);
void trierarch_explicit_sync_state_destroy(struct trierarch_explicit_sync_state *state);

/* The release object is frame-owned after take_release(). Completion is safe
 * only after the renderer has finished every read from that frame. */
void trierarch_explicit_release_complete(struct trierarch_explicit_release *release);

void trierarch_explicit_sync_bind(struct wl_client *client, void *data,
        uint32_t version, uint32_t id);

#endif
