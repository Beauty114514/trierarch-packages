#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { DEFAULT_OBSERVE_MS = 10000, POLL_MS = 100 };

static volatile sig_atomic_t running = 1;

static void stop(int signal_number) {
    (void)signal_number;
    running = 0;
}

static void log_line(FILE *log, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    vfprintf(log, format, arguments);
    va_end(arguments);
    fputc('\n', log);
    fflush(log);
}

static bool is_socket(const char *directory, const char *name) {
    char path[PATH_MAX];
    int length = snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (length < 0 || (size_t)length >= sizeof(path)) return false;
    struct stat metadata;
    return lstat(path, &metadata) == 0 && S_ISSOCK(metadata.st_mode);
}

static void observe_sockets(FILE *log, const char *runtime_directory, char **known, size_t *known_count) {
    DIR *directory = opendir(runtime_directory);
    if (!directory) return;

    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (entry->d_name[0] == '.' || !is_socket(runtime_directory, entry->d_name)) continue;
        bool already_known = false;
        for (size_t index = 0; index < *known_count; index++) {
            if (strcmp(known[index], entry->d_name) == 0) {
                already_known = true;
                break;
            }
        }
        if (already_known || *known_count == 32) continue;
        known[*known_count] = strdup(entry->d_name);
        if (known[*known_count] == NULL) continue;
        (*known_count)++;
        log_line(log, "observed Wayland socket: %s/%s", runtime_directory, entry->d_name);
    }
    closedir(directory);
}

static long parse_milliseconds(const char *value) {
    char *end = NULL;
    errno = 0;
    long milliseconds = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || milliseconds < 0 || milliseconds > 60000) {
        return -1;
    }
    return milliseconds;
}

int main(int argc, char **argv) {
    const char *runtime_directory = NULL;
    const char *log_path = NULL;
    long observe_milliseconds = DEFAULT_OBSERVE_MS;

    for (int index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--runtime-dir") == 0 && index + 1 < argc) {
            runtime_directory = argv[++index];
        } else if (strcmp(argv[index], "--log") == 0 && index + 1 < argc) {
            log_path = argv[++index];
        } else if (strcmp(argv[index], "--observe-ms") == 0 && index + 1 < argc) {
            observe_milliseconds = parse_milliseconds(argv[++index]);
        } else {
            fprintf(stderr, "usage: %s --runtime-dir DIRECTORY --log FILE [--observe-ms 0..60000]\n", argv[0]);
            return 2;
        }
    }
    if (!runtime_directory || !log_path || observe_milliseconds < 0) {
        fprintf(stderr, "runtime directory, log file, and valid observation duration are required\n");
        return 2;
    }

    FILE *log = fopen(log_path, "a");
    if (!log) {
        perror("open supervisor log");
        return 1;
    }
    signal(SIGTERM, stop);
    signal(SIGINT, stop);
    char *known[32] = {0};
    size_t known_count = 0;
    log_line(log, "session supervisor started; runtime=%s observe_ms=%ld", runtime_directory, observe_milliseconds);
    for (long elapsed = 0; running && elapsed <= observe_milliseconds; elapsed += POLL_MS) {
        observe_sockets(log, runtime_directory, known, &known_count);
        if (elapsed == observe_milliseconds) break;
        struct timespec delay = {.tv_sec = 0, .tv_nsec = POLL_MS * 1000000L};
        nanosleep(&delay, NULL);
    }
    log_line(log, "session supervisor observation finished; sockets=%zu", known_count);
    for (size_t index = 0; index < known_count; index++) free(known[index]);
    fclose(log);
    return 0;
}
