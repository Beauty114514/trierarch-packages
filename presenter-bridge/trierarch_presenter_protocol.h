#ifndef TRIERARCH_PRESENTER_PROTOCOL_H
#define TRIERARCH_PRESENTER_PROTOCOL_H

#include <stdint.h>

/*
 * Local companion ABI between the Android presenter and a guest compositor
 * backend.  It is not a Wayland protocol and is never visible to desktop
 * clients.  The transport is SOCK_SEQPACKET; file descriptors use SCM_RIGHTS.
 */
#define TRIERARCH_PRESENTER_MAGIC 0x54525052u /* TRPR */
#define TRIERARCH_PRESENTER_VERSION 1u

enum trierarch_presenter_message_type {
    TRIERARCH_PRESENTER_HELLO = 1,
    TRIERARCH_PRESENTER_BUFFER = 2,
    TRIERARCH_PRESENTER_RENDER = 3,
    TRIERARCH_PRESENTER_FRAME_DONE = 4,
    TRIERARCH_PRESENTER_RESET = 5,
    TRIERARCH_PRESENTER_ERROR = 6,
};

struct trierarch_presenter_header {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
};

/* Sent by the guest backend immediately after connecting. */
struct trierarch_presenter_hello {
    struct trierarch_presenter_header header;
};

/*
 * Announces an Android-owned Surface buffer.  One pixel-plane dma-buf FD
 * accompanies this message.  The FD is a duplicate for the guest only; the
 * Android presenter retains the ANativeWindowBuffer and its queue ownership.
 */
struct trierarch_presenter_buffer {
    struct trierarch_presenter_header header;
    uint32_t generation;
    uint32_t slot_id;
    uint32_t width;
    uint32_t height;
    uint32_t drm_format;
    uint32_t stride;
    uint32_t offset;
    uint64_t modifier;
};

/*
 * The Android presenter has dequeued this slot and requests one rendered
 * frame.  An optional acquire sync_file FD accompanies the message.  A slot
 * is valid only in the named generation and until RESET or disconnect.
 */
struct trierarch_presenter_render {
    struct trierarch_presenter_header header;
    uint32_t generation;
    uint32_t slot_id;
};

/*
 * The guest completed rendering the requested slot.  It must return exactly
 * one optional release sync_file FD, produced after all GPU writes.  The host
 * queues that same Android buffer only after receiving this message.
 */
struct trierarch_presenter_frame_done {
    struct trierarch_presenter_header header;
    uint32_t generation;
    uint32_t slot_id;
};

/* Invalidates every slot in the previous generation.  No FDs accompany it. */
struct trierarch_presenter_reset {
    struct trierarch_presenter_header header;
    uint32_t generation;
    uint32_t width;
    uint32_t height;
};

struct trierarch_presenter_error {
    struct trierarch_presenter_header header;
    uint32_t code;
};

#endif
