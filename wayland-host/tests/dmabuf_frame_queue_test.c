#include "dmabuf_frame_queue.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <unistd.h>

static unsigned int destroyed;

static void record_destroy(void *data) {
    assert(data == (void *)(uintptr_t)0x1234);
    destroyed++;
}

static int make_fd(void) {
    int fds[2] = {-1, -1};
    assert(pipe(fds) == 0);
    close(fds[1]);
    return fds[0];
}

static void test_destroy_before_frame_retire(void) {
    int canonical_fd = make_fd();
    struct trierarch_dmabuf_record *record = trierarch_dmabuf_record_create(
            canonical_fd, (void *)(uintptr_t)0x1234, record_destroy);
    assert(record);
    int fence = eventfd(0, EFD_CLOEXEC);
    assert(fence >= 0);
    struct trierarch_dmabuf_frame *frame = trierarch_dmabuf_frame_create(record, fence, 1);
    assert(frame);
    assert(trierarch_dmabuf_frame_fd(frame) >= 0);

    /* Simulates wl_buffer.destroy: the resource reference disappears first. */
    trierarch_dmabuf_record_unref(record);
    assert(destroyed == 0);
    assert(fcntl(trierarch_dmabuf_frame_fd(frame), F_GETFD) >= 0);
    assert(fcntl(trierarch_dmabuf_frame_acquire_fence_fd(frame), F_GETFD) >= 0);

    trierarch_dmabuf_frame_unref(frame);
    assert(destroyed == 1);
    errno = 0;
    assert(fcntl(canonical_fd, F_GETFD) == -1 && errno == EBADF);
}

static void test_latest_retires_older_frames(void) {
    int canonical_fd = make_fd();
    struct trierarch_dmabuf_record *record = trierarch_dmabuf_record_create(
            canonical_fd, (void *)(uintptr_t)0x1234, record_destroy);
    assert(record);
    struct trierarch_dmabuf_frame_queue *queue = trierarch_dmabuf_frame_queue_create();
    assert(queue);

    int old_fence = eventfd(0, EFD_CLOEXEC);
    int latest_fence = eventfd(0, EFD_CLOEXEC);
    assert(old_fence >= 0 && latest_fence >= 0);
    struct trierarch_dmabuf_frame *old = trierarch_dmabuf_frame_create(record, old_fence, 10);
    struct trierarch_dmabuf_frame *latest = trierarch_dmabuf_frame_create(record, latest_fence, 11);
    assert(old && latest);
    assert(trierarch_dmabuf_frame_queue_push(queue, old));
    assert(trierarch_dmabuf_frame_queue_push(queue, latest));
    assert(trierarch_dmabuf_frame_queue_size(queue) == 2);

    struct trierarch_dmabuf_frame *taken = trierarch_dmabuf_frame_queue_take_latest(queue);
    assert(taken == latest);
    assert(trierarch_dmabuf_frame_sequence(taken) == 11);
    assert(trierarch_dmabuf_frame_queue_size(queue) == 0);
    errno = 0;
    assert(fcntl(old_fence, F_GETFD) == -1 && errno == EBADF);
    assert(fcntl(trierarch_dmabuf_frame_acquire_fence_fd(taken), F_GETFD) >= 0);

    trierarch_dmabuf_frame_unref(taken);
    trierarch_dmabuf_frame_queue_destroy(queue);
    trierarch_dmabuf_record_unref(record);
    assert(destroyed == 2);
}

static void test_queue_clear_releases_everything(void) {
    int canonical_fd = make_fd();
    struct trierarch_dmabuf_record *record = trierarch_dmabuf_record_create(
            canonical_fd, (void *)(uintptr_t)0x1234, record_destroy);
    assert(record);
    struct trierarch_dmabuf_frame_queue *queue = trierarch_dmabuf_frame_queue_create();
    assert(queue);
    for (uint64_t sequence = 0; sequence < 3; sequence++) {
        struct trierarch_dmabuf_frame *frame = trierarch_dmabuf_frame_create(record,
                eventfd(0, EFD_CLOEXEC), sequence);
        assert(frame);
        assert(trierarch_dmabuf_frame_queue_push(queue, frame));
    }
    trierarch_dmabuf_frame_queue_clear(queue);
    assert(trierarch_dmabuf_frame_queue_size(queue) == 0);
    trierarch_dmabuf_frame_queue_destroy(queue);
    trierarch_dmabuf_record_unref(record);
    assert(destroyed == 3);
}

int main(void) {
    test_destroy_before_frame_retire();
    test_latest_retires_older_frames();
    test_queue_clear_releases_everything();
    puts("dmabuf_frame_queue_test: ok");
    return 0;
}
