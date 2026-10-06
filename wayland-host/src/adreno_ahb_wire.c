#include "adreno_ahb_wire.h"

#include <android/hardware_buffer.h>
#include <android/log.h>

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#define TAG "TrierarchAdrenoAhb"
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

enum { GRAPHIC_BUFFER_HEADER_INTS = 13, MAX_NATIVE_HANDLE_INTS = 64 };

static int send_graphic_buffer(int socket_fd, const int32_t *wire, size_t bytes,
        const int fds[2]) {
    char control[CMSG_SPACE(2 * sizeof(int))] = {0};
    struct iovec iov = { .iov_base = (void *)wire, .iov_len = bytes };
    struct msghdr message = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = control,
        .msg_controllen = sizeof(control),
    };
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(2 * sizeof(int));
    memcpy(CMSG_DATA(cmsg), fds, 2 * sizeof(int));
    message.msg_controllen = cmsg->cmsg_len;
    return sendmsg(socket_fd, &message, 0) == (ssize_t)bytes ? 0 : -1;
}

bool trierarch_adreno_ahb_register_handle(const AHardwareBuffer_Desc *description,
        const struct trierarch_adreno_native_handle *handle, int pixel_fd,
        int metadata_fd, AHardwareBuffer **received) {
    if (!description || !handle || !received || pixel_fd < 0 || metadata_fd < 0 ||
            handle->num_fds != 2 || handle->num_ints < 0 ||
            handle->num_ints > MAX_NATIVE_HANDLE_INTS)
        return false;
    *received = NULL;
    int fds[2] = { dup(pixel_fd), dup(metadata_fd) };
    int sockets[2] = { -1, -1 };
    int32_t *wire = NULL;
    int send_result = -1;
    int receive_result = -1;
    int saved_errno = 0;
    bool success = false;
    if (fds[0] < 0 || fds[1] < 0 ||
            socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) {
        saved_errno = errno;
        goto out;
    }

    const size_t wire_ints = GRAPHIC_BUFFER_HEADER_INTS + (size_t)handle->num_ints;
    wire = calloc(wire_ints, sizeof(*wire));
    if (!wire)
        goto out;
    static _Atomic uint32_t counter;
    uint64_t id = ((uint64_t)getpid() << 32) |
            atomic_fetch_add_explicit(&counter, 1, memory_order_relaxed) | (1ULL << 63);
    wire[0] = 0x47423031; /* 'GB01': GraphicBuffer::flatten wire marker. */
    wire[1] = (int32_t)description->width;
    wire[2] = (int32_t)description->height;
    wire[3] = (int32_t)description->stride;
    wire[4] = (int32_t)description->format;
    wire[5] = (int32_t)description->layers;
    wire[6] = (int32_t)description->usage;
    wire[7] = (int32_t)(id >> 32);
    wire[8] = (int32_t)id;
    wire[9] = 0;
    wire[10] = 2;
    wire[11] = handle->num_ints;
    wire[12] = (int32_t)(description->usage >> 32);
    memcpy(wire + GRAPHIC_BUFFER_HEADER_INTS, &handle->data[handle->num_fds],
            (size_t)handle->num_ints * sizeof(int));
    send_result = send_graphic_buffer(sockets[0], wire, wire_ints * sizeof(*wire), fds);
    if (send_result != 0) {
        saved_errno = errno;
        goto out;
    }
    receive_result = AHardwareBuffer_recvHandleFromUnixSocket(sockets[1], received);
    if (receive_result != 0 || !*received) {
        saved_errno = errno;
        goto out;
    }
    success = true;

out:
    free(wire);
    if (sockets[0] >= 0)
        close(sockets[0]);
    if (sockets[1] >= 0)
        close(sockets[1]);
    if (fds[0] >= 0)
        close(fds[0]);
    if (fds[1] >= 0)
        close(fds[1]);
    if (!success)
        LOGW("AHB GB01 handle registration failed: send=%d receive=%d errno=%d",
                send_result, receive_result, saved_errno);
    return success;
}
