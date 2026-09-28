#include "dmabuf_presentation.h"

#include "dmabuf_fence_watch.h"
#include "dmabuf_frame_queue.h"
#include "server_internal.h"

#include <android/log.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define TRIERARCH_TAG "TrierarchWayland"

struct dmabuf_retire_data {
    struct compositor_surface *surface;
    struct shm_buffer *buffer;
    struct trierarch_explicit_release *release;
};

static void pending_fence_ready(void *data, uint32_t mask) {
    struct compositor_surface *surface = data;
    if (!surface)
        return;
    if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) {
        __android_log_print(ANDROID_LOG_WARN, TRIERARCH_TAG,
                "dma-buf fence event has error flags: surface=%u mask=0x%x",
                wl_resource_get_id(surface->wl_surface), mask);
    }
    if (mask & (WL_EVENT_READABLE | WL_EVENT_ERROR | WL_EVENT_HANGUP))
        trierarch_wayland_request_render(surface->server);
}

static void retire_frame(void *data) {
    struct dmabuf_retire_data *retire = data;
    if (!retire)
        return;
    trierarch_dmabuf_buffer_retire_presentation(retire->buffer);
    if (retire->release && retire->surface->dmabuf_release_pending)
        retire->surface->dmabuf_release_pending--;
    trierarch_explicit_release_complete(retire->release);
    free(retire);
}

bool trierarch_dmabuf_surface_submit(struct compositor_surface *surface,
        struct shm_buffer *buffer) {
    if (!surface || !buffer || !buffer->dmabuf || !buffer->dmabuf_record)
        return false;
    if (!surface->dmabuf_frames) {
        surface->dmabuf_frames = trierarch_dmabuf_frame_queue_create();
        if (!surface->dmabuf_frames)
            return false;
    }
    int acquire_fence_fd = trierarch_explicit_sync_state_take_acquire_fence(
            &surface->explicit_sync);
    struct trierarch_explicit_release *release = trierarch_explicit_sync_state_take_release(
            &surface->explicit_sync);
    struct trierarch_dmabuf_frame *frame = trierarch_dmabuf_frame_create(
            buffer->dmabuf_record, acquire_fence_fd, ++surface->dmabuf_frame_sequence);
    if (!frame) {
        trierarch_explicit_release_complete(release);
        return false;
    }
    struct dmabuf_retire_data *retire = calloc(1, sizeof(*retire));
    if (!retire) {
        trierarch_dmabuf_frame_unref(frame);
        trierarch_explicit_release_complete(release);
        return false;
    }
    retire->surface = surface;
    retire->buffer = buffer;
    retire->release = release;
    if (release)
        surface->dmabuf_release_pending++;
    buffer->dmabuf_presentation_uses++;
    trierarch_dmabuf_frame_set_retire_callback(frame, retire_frame, retire);
    if (trierarch_dmabuf_frame_queue_push(surface->dmabuf_frames, frame))
        return true;
    trierarch_dmabuf_frame_unref(frame);
    return false;
}

void trierarch_dmabuf_surface_retire(struct compositor_surface *surface) {
    if (!surface)
        return;
    trierarch_dmabuf_fence_watch_cancel(&surface->dmabuf_fence_watch);
    bool had_presented_frame = surface->dmabuf_presented_frame != NULL;
    size_t queued_frames = surface->dmabuf_frames
            ? trierarch_dmabuf_frame_queue_size(surface->dmabuf_frames) : 0;
    if (surface->dmabuf_presented_frame) {
        trierarch_dmabuf_frame_unref(surface->dmabuf_presented_frame);
        surface->dmabuf_presented_frame = NULL;
    }
    if (surface->dmabuf_frames)
        trierarch_dmabuf_frame_queue_clear(surface->dmabuf_frames);
    surface->dmabuf_frame_pending = false;
    if (had_presented_frame || queued_frames) {
        __android_log_print(ANDROID_LOG_INFO, TRIERARCH_TAG,
                "dma-buf presentation retired: surface=%u active=%d queued=%zu",
                wl_resource_get_id(surface->wl_surface), had_presented_frame, queued_frames);
    }
}

void trierarch_dmabuf_surface_watch_pending_fence(struct compositor_surface *surface) {
    if (!surface || trierarch_dmabuf_fence_watch_is_armed(&surface->dmabuf_fence_watch) ||
            !surface->server ||
            !surface->server->event_loop || !surface->dmabuf_frames)
        return;
    int fd = trierarch_dmabuf_frame_queue_dup_head_readiness_fd(surface->dmabuf_frames);
    if (fd < 0)
        return;
    if (!trierarch_dmabuf_fence_watch_arm(&surface->dmabuf_fence_watch,
            surface->server->event_loop, fd, pending_fence_ready, surface)) {
        __android_log_print(ANDROID_LOG_WARN, TRIERARCH_TAG,
                "unable to watch pending dma-buf fence: surface=%u: %s",
                wl_resource_get_id(surface->wl_surface), strerror(errno));
    }
}

void trierarch_dmabuf_surface_destroy(struct compositor_surface *surface) {
    if (!surface)
        return;
    trierarch_dmabuf_surface_retire(surface);
    if (surface->dmabuf_frames) {
        trierarch_dmabuf_frame_queue_destroy(surface->dmabuf_frames);
        surface->dmabuf_frames = NULL;
    }
}

void trierarch_dmabuf_surface_latch_frames(struct wayland_server *server) {
    if (!server)
        return;
    struct compositor_surface *surface;
    wl_list_for_each(surface, &server->surfaces, link) {
        enum trierarch_dmabuf_frame_readiness readiness = TRIERARCH_DMABUF_FRAME_READY;
        struct trierarch_dmabuf_frame *latest = surface->dmabuf_frames
                ? trierarch_dmabuf_frame_queue_take_latest_ready(surface->dmabuf_frames,
                        &readiness) : NULL;
        if (!latest) {
            if (readiness == TRIERARCH_DMABUF_FRAME_PENDING &&
                    !surface->dmabuf_frame_pending) {
                surface->dmabuf_frame_pending = true;
                __android_log_print(ANDROID_LOG_INFO, TRIERARCH_TAG,
                        "dma-buf frame pending: surface=%u",
                        wl_resource_get_id(surface->wl_surface));
            }
            if (readiness == TRIERARCH_DMABUF_FRAME_PENDING)
                trierarch_dmabuf_surface_watch_pending_fence(surface);
            continue;
        }
        trierarch_dmabuf_fence_watch_cancel(&surface->dmabuf_fence_watch);
        surface->dmabuf_frame_pending = false;
        if (surface->dmabuf_presented_frame)
            trierarch_dmabuf_frame_unref(surface->dmabuf_presented_frame);
        surface->dmabuf_presented_frame = latest;
    }
}
