#include "explicit_sync.h"

#include <unistd.h>

static void close_fence(int *fd) {
    if (*fd >= 0)
        close(*fd);
    *fd = -1;
}

void trierarch_explicit_sync_state_init(struct trierarch_explicit_sync_state *state) {
    if (!state)
        return;
    state->pending_acquire_fence_fd = -1;
    state->current_acquire_fence_fd = -1;
    state->pending_release = NULL;
    state->current_release = NULL;
}

void trierarch_explicit_sync_state_commit(struct trierarch_explicit_sync_state *state,
        bool attached) {
    if (!state || !attached)
        return;
    close_fence(&state->current_acquire_fence_fd);
    state->current_acquire_fence_fd = state->pending_acquire_fence_fd;
    state->pending_acquire_fence_fd = -1;
    state->current_release = state->pending_release;
    state->pending_release = NULL;
}

void trierarch_explicit_sync_state_destroy(struct trierarch_explicit_sync_state *state) {
    if (!state)
        return;
    close_fence(&state->pending_acquire_fence_fd);
    close_fence(&state->current_acquire_fence_fd);
    state->pending_release = NULL;
    state->current_release = NULL;
}
