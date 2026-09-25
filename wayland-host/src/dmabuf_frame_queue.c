#include "dmabuf_frame_queue.h"

#include <stdatomic.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

struct trierarch_dmabuf_record {
    atomic_uint references;
    int canonical_fd;
    void *data;
    trierarch_dmabuf_record_destroy_fn destroy;
};

struct trierarch_dmabuf_frame {
    atomic_uint references;
    struct trierarch_dmabuf_record *record;
    int fd;
    int acquire_fence_fd;
    uint64_t sequence;
    struct trierarch_dmabuf_frame *next;
};

struct trierarch_dmabuf_frame_queue {
    pthread_mutex_t lock;
    struct trierarch_dmabuf_frame *head;
    struct trierarch_dmabuf_frame *tail;
    size_t size;
};

static int duplicate_fd(int fd) {
#ifdef F_DUPFD_CLOEXEC
    return fcntl(fd, F_DUPFD_CLOEXEC, 0);
#else
    int copy = dup(fd);
    if (copy >= 0)
        (void)fcntl(copy, F_SETFD, FD_CLOEXEC);
    return copy;
#endif
}

struct trierarch_dmabuf_record *trierarch_dmabuf_record_create(
        int canonical_fd, void *data, trierarch_dmabuf_record_destroy_fn destroy) {
    if (canonical_fd < 0)
        return NULL;
    struct trierarch_dmabuf_record *record = calloc(1, sizeof(*record));
    if (!record) {
        close(canonical_fd);
        return NULL;
    }
    atomic_init(&record->references, 1);
    record->canonical_fd = canonical_fd;
    record->data = data;
    record->destroy = destroy;
    return record;
}

void trierarch_dmabuf_record_ref(struct trierarch_dmabuf_record *record) {
    if (record)
        atomic_fetch_add_explicit(&record->references, 1, memory_order_relaxed);
}

void trierarch_dmabuf_record_unref(struct trierarch_dmabuf_record *record) {
    if (!record || atomic_fetch_sub_explicit(&record->references, 1,
            memory_order_acq_rel) != 1)
        return;
    if (record->destroy)
        record->destroy(record->data);
    if (record->canonical_fd >= 0)
        close(record->canonical_fd);
    free(record);
}

int trierarch_dmabuf_record_fd(const struct trierarch_dmabuf_record *record) {
    return record ? record->canonical_fd : -1;
}

void *trierarch_dmabuf_record_data(const struct trierarch_dmabuf_record *record) {
    return record ? record->data : NULL;
}

struct trierarch_dmabuf_frame *trierarch_dmabuf_frame_create(
        struct trierarch_dmabuf_record *record, int acquire_fence_fd,
        uint64_t sequence) {
    if (!record) {
        if (acquire_fence_fd >= 0)
            close(acquire_fence_fd);
        return NULL;
    }
    int frame_fd = duplicate_fd(record->canonical_fd);
    if (frame_fd < 0) {
        if (acquire_fence_fd >= 0)
            close(acquire_fence_fd);
        return NULL;
    }
    struct trierarch_dmabuf_frame *frame = calloc(1, sizeof(*frame));
    if (!frame) {
        close(frame_fd);
        if (acquire_fence_fd >= 0)
            close(acquire_fence_fd);
        return NULL;
    }
    atomic_init(&frame->references, 1);
    trierarch_dmabuf_record_ref(record);
    frame->record = record;
    frame->fd = frame_fd;
    frame->acquire_fence_fd = acquire_fence_fd;
    frame->sequence = sequence;
    return frame;
}

void trierarch_dmabuf_frame_ref(struct trierarch_dmabuf_frame *frame) {
    if (frame)
        atomic_fetch_add_explicit(&frame->references, 1, memory_order_relaxed);
}

void trierarch_dmabuf_frame_unref(struct trierarch_dmabuf_frame *frame) {
    if (!frame || atomic_fetch_sub_explicit(&frame->references, 1,
            memory_order_acq_rel) != 1)
        return;
    if (frame->fd >= 0)
        close(frame->fd);
    if (frame->acquire_fence_fd >= 0)
        close(frame->acquire_fence_fd);
    trierarch_dmabuf_record_unref(frame->record);
    free(frame);
}

int trierarch_dmabuf_frame_fd(const struct trierarch_dmabuf_frame *frame) {
    return frame ? frame->fd : -1;
}

int trierarch_dmabuf_frame_acquire_fence_fd(const struct trierarch_dmabuf_frame *frame) {
    return frame ? frame->acquire_fence_fd : -1;
}

uint64_t trierarch_dmabuf_frame_sequence(const struct trierarch_dmabuf_frame *frame) {
    return frame ? frame->sequence : 0;
}

struct trierarch_dmabuf_record *trierarch_dmabuf_frame_record(
        const struct trierarch_dmabuf_frame *frame) {
    return frame ? frame->record : NULL;
}

struct trierarch_dmabuf_frame_queue *trierarch_dmabuf_frame_queue_create(void) {
    struct trierarch_dmabuf_frame_queue *queue = calloc(1, sizeof(*queue));
    if (!queue)
        return NULL;
    if (pthread_mutex_init(&queue->lock, NULL) != 0) {
        free(queue);
        return NULL;
    }
    return queue;
}

void trierarch_dmabuf_frame_queue_clear(struct trierarch_dmabuf_frame_queue *queue) {
    if (!queue)
        return;
    pthread_mutex_lock(&queue->lock);
    struct trierarch_dmabuf_frame *head = queue->head;
    queue->head = NULL;
    queue->tail = NULL;
    queue->size = 0;
    pthread_mutex_unlock(&queue->lock);
    while (head) {
        struct trierarch_dmabuf_frame *next = head->next;
        head->next = NULL;
        trierarch_dmabuf_frame_unref(head);
        head = next;
    }
}

void trierarch_dmabuf_frame_queue_destroy(struct trierarch_dmabuf_frame_queue *queue) {
    if (!queue)
        return;
    trierarch_dmabuf_frame_queue_clear(queue);
    pthread_mutex_destroy(&queue->lock);
    free(queue);
}

bool trierarch_dmabuf_frame_queue_push(struct trierarch_dmabuf_frame_queue *queue,
        struct trierarch_dmabuf_frame *frame) {
    if (!queue || !frame)
        return false;
    frame->next = NULL;
    pthread_mutex_lock(&queue->lock);
    if (queue->tail)
        queue->tail->next = frame;
    else
        queue->head = frame;
    queue->tail = frame;
    queue->size++;
    pthread_mutex_unlock(&queue->lock);
    return true;
}

struct trierarch_dmabuf_frame *trierarch_dmabuf_frame_queue_take_next(
        struct trierarch_dmabuf_frame_queue *queue) {
    if (!queue)
        return NULL;
    pthread_mutex_lock(&queue->lock);
    struct trierarch_dmabuf_frame *frame = queue->head;
    if (frame) {
        queue->head = frame->next;
        if (!queue->head)
            queue->tail = NULL;
        frame->next = NULL;
        queue->size--;
    }
    pthread_mutex_unlock(&queue->lock);
    return frame;
}

struct trierarch_dmabuf_frame *trierarch_dmabuf_frame_queue_take_latest(
        struct trierarch_dmabuf_frame_queue *queue) {
    if (!queue)
        return NULL;
    pthread_mutex_lock(&queue->lock);
    struct trierarch_dmabuf_frame *head = queue->head;
    struct trierarch_dmabuf_frame *latest = queue->tail;
    queue->head = NULL;
    queue->tail = NULL;
    queue->size = 0;
    pthread_mutex_unlock(&queue->lock);
    if (!latest)
        return NULL;
    while (head && head != latest) {
        struct trierarch_dmabuf_frame *next = head->next;
        head->next = NULL;
        trierarch_dmabuf_frame_unref(head);
        head = next;
    }
    latest->next = NULL;
    return latest;
}

size_t trierarch_dmabuf_frame_queue_size(struct trierarch_dmabuf_frame_queue *queue) {
    if (!queue)
        return 0;
    pthread_mutex_lock(&queue->lock);
    size_t size = queue->size;
    pthread_mutex_unlock(&queue->lock);
    return size;
}
