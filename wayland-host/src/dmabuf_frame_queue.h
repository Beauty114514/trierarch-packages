#ifndef TRIERARCH_DMABUF_FRAME_QUEUE_H
#define TRIERARCH_DMABUF_FRAME_QUEUE_H

/*
 * An ownership-only building block for dma-buf presentation.
 *
 * This module deliberately knows nothing about Wayland resources, Android,
 * EGL, or a renderer.  It separates a client-visible buffer object's lifetime
 * from the lifetime of every submitted frame which still reads that object.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct trierarch_dmabuf_record;
struct trierarch_dmabuf_frame;
struct trierarch_dmabuf_frame_queue;

/* Called exactly once, immediately before the record's canonical fd is closed.
 * `data` is owned by the caller and is never interpreted by this module. */
typedef void (*trierarch_dmabuf_record_destroy_fn)(void *data);

/* Takes ownership of canonical_fd. `data` and destroy are for the future
 * Wayland/import adapter; they permit it to tear down side data at the same
 * point as the canonical fd. Returns NULL after closing canonical_fd on error. */
struct trierarch_dmabuf_record *trierarch_dmabuf_record_create(
        int canonical_fd, void *data, trierarch_dmabuf_record_destroy_fn destroy);
void trierarch_dmabuf_record_ref(struct trierarch_dmabuf_record *record);
void trierarch_dmabuf_record_unref(struct trierarch_dmabuf_record *record);
int trierarch_dmabuf_record_fd(const struct trierarch_dmabuf_record *record);
void *trierarch_dmabuf_record_data(const struct trierarch_dmabuf_record *record);

/* Takes ownership of acquire_fence_fd (-1 means implicit synchronization) and
 * duplicates the record fd for this frame.  The returned frame owns both fds
 * and one record reference. */
struct trierarch_dmabuf_frame *trierarch_dmabuf_frame_create(
        struct trierarch_dmabuf_record *record, int acquire_fence_fd,
        uint64_t sequence);
void trierarch_dmabuf_frame_ref(struct trierarch_dmabuf_frame *frame);
void trierarch_dmabuf_frame_unref(struct trierarch_dmabuf_frame *frame);
int trierarch_dmabuf_frame_fd(const struct trierarch_dmabuf_frame *frame);
int trierarch_dmabuf_frame_acquire_fence_fd(const struct trierarch_dmabuf_frame *frame);
uint64_t trierarch_dmabuf_frame_sequence(const struct trierarch_dmabuf_frame *frame);
struct trierarch_dmabuf_record *trierarch_dmabuf_frame_record(
        const struct trierarch_dmabuf_frame *frame);

struct trierarch_dmabuf_frame_queue *trierarch_dmabuf_frame_queue_create(void);
void trierarch_dmabuf_frame_queue_destroy(struct trierarch_dmabuf_frame_queue *queue);

/* On success consumes the caller's frame reference. On failure the caller
 * retains it. Queue operations are mutex-protected and may be called from a
 * producer and a consumer thread. */
bool trierarch_dmabuf_frame_queue_push(struct trierarch_dmabuf_frame_queue *queue,
        struct trierarch_dmabuf_frame *frame);

/* Transfers the oldest queued frame reference to the caller, or returns NULL.
 * This function intentionally does not wait on the acquire fence. */
struct trierarch_dmabuf_frame *trierarch_dmabuf_frame_queue_take_next(
        struct trierarch_dmabuf_frame_queue *queue);

/* Transfers the newest queued frame reference to the caller and retires all
 * older queued frames. This is useful for a mailbox presentation policy. */
struct trierarch_dmabuf_frame *trierarch_dmabuf_frame_queue_take_latest(
        struct trierarch_dmabuf_frame_queue *queue);

void trierarch_dmabuf_frame_queue_clear(struct trierarch_dmabuf_frame_queue *queue);
size_t trierarch_dmabuf_frame_queue_size(struct trierarch_dmabuf_frame_queue *queue);

#endif
