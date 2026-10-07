#define _GNU_SOURCE
#include "trierarch_presenter_protocol.h"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int connect_socket(const char *path) {
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(address.sun_path)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(address.sun_path, path);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int parse_hold_seconds(const char *text, int *seconds) {
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno || !end || *end || value < 1 || value > 300)
        return -1;
    *seconds = (int)value;
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2 && argc != 3) {
        fprintf(stderr, "usage: %s SOCKET [HOLD_SECONDS]\n", argv[0]);
        return EXIT_FAILURE;
    }
    int hold_seconds = 15;
    if (argc == 3 && parse_hold_seconds(argv[2], &hold_seconds) < 0) {
        fprintf(stderr, "HOLD_SECONDS must be between 1 and 300\n");
        return EXIT_FAILURE;
    }

    int fd = connect_socket(argv[1]);
    if (fd < 0) {
        perror("presenter-hello: connect");
        return EXIT_FAILURE;
    }
    const struct trierarch_presenter_hello hello = {
        .header = {
            .magic = TRIERARCH_PRESENTER_MAGIC,
            .version = TRIERARCH_PRESENTER_VERSION,
            .type = TRIERARCH_PRESENTER_HELLO,
        },
    };
    if (send(fd, &hello, sizeof(hello), MSG_NOSIGNAL) != (ssize_t)sizeof(hello)) {
        perror("presenter-hello: send HELLO");
        close(fd);
        return EXIT_FAILURE;
    }
    printf("presenter-hello: HELLO sent; holding connection for %d seconds\n",
            hold_seconds);
    fflush(stdout);

    struct pollfd poll_fd = { .fd = fd, .events = POLLIN | POLLERR | POLLHUP };
    int result = poll(&poll_fd, 1, hold_seconds * 1000);
    if (result == 0) {
        puts("presenter-hello: host kept the handshake open");
        close(fd);
        return EXIT_SUCCESS;
    }
    if (result < 0) {
        perror("presenter-hello: poll");
        close(fd);
        return EXIT_FAILURE;
    }

    struct trierarch_presenter_error error = {0};
    ssize_t received = recv(fd, &error, sizeof(error), 0);
    close(fd);
    if (received == (ssize_t)sizeof(error) &&
            error.header.magic == TRIERARCH_PRESENTER_MAGIC &&
            error.header.version == TRIERARCH_PRESENTER_VERSION &&
            error.header.type == TRIERARCH_PRESENTER_ERROR) {
        fprintf(stderr, "presenter-hello: host rejected handshake: error=%u\n",
                error.code);
    } else {
        fprintf(stderr, "presenter-hello: host closed the connection\n");
    }
    return EXIT_FAILURE;
}
