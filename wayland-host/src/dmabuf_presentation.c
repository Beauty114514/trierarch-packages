#include "dmabuf_presentation.h"

#include "dmabuf_frame_queue.h"
#include "server_internal.h"

bool trierarch_dmabuf_surface_submit(struct compositor_surface *surface,
        struct shm_buffer *buffer) {
    if (!surface || !buffer || !buffer->dmabuf || !buffer->dmabuf_record)
        return false;
    if (!surface->dmabuf_frames) {
        surface->dmabuf_frames = trierarch_dmabuf_frame_queue_create();
        if (!surface->dmabuf_frames)
            return false;
    }
    struct trierarch_dmabuf_frame *frame = trierarch_dmabuf_frame_create(
            buffer->dmabuf_record, -1, ++surface->dmabuf_frame_sequence);
    if (!frame)
        return false;
    buffer->dmabuf_presentation_uses++;
    trierarch_dmabuf_frame_set_retire_callback(frame,
            trierarch_dmabuf_buffer_retire_presentation, buffer);
    if (trierarch_dmabuf_frame_queue_push(surface->dmabuf_frames, frame))
        return true;
    trierarch_dmabuf_frame_unref(frame);
    return false;
}

void trierarch_dmabuf_surface_retire(struct compositor_surface *surface) {
    if (!surface)
        return;
    if (surface->dmabuf_presented_frame) {
        trierarch_dmabuf_frame_unref(surface->dmabuf_presented_frame);
        surface->dmabuf_presented_frame = NULL;
    }
    if (surface->dmabuf_frames)
        trierarch_dmabuf_frame_queue_clear(surface->dmabuf_frames);
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
        struct trierarch_dmabuf_frame *latest = surface->dmabuf_frames
                ? trierarch_dmabuf_frame_queue_take_latest(surface->dmabuf_frames) : NULL;
        if (!latest)
            continue;
        if (surface->dmabuf_presented_frame)
            trierarch_dmabuf_frame_unref(surface->dmabuf_presented_frame);
        surface->dmabuf_presented_frame = latest;
    }
}
