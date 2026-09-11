#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { DEFAULT_OBSERVE_MS = 10000, PLASMA_GRACE_MS = 4000, POLL_MS = 100, MAX_KNOWN_SOCKETS = 32, MAX_ENVIRONMENT_BYTES = 131072 };

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t managed_child = 0;

static void stop(int signal_number) {
    (void)signal_number;
    running = 0;
    if (managed_child > 0) kill((pid_t)managed_child, SIGTERM);
}

static void log_line(FILE *log, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    vfprintf(log, format, arguments);
    va_end(arguments);
    fputc('\n', log);
    fflush(log);
}

static void sleep_milliseconds(long milliseconds) {
    struct timespec delay = {.tv_sec = milliseconds / 1000, .tv_nsec = (milliseconds % 1000) * 1000000L};
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR && running) {}
}

static bool is_decimal(const char *value) {
    if (*value == '\0') return false;
    for (; *value; value++) if (*value < '0' || *value > '9') return false;
    return true;
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
            if (strcmp(known[index], entry->d_name) == 0) { already_known = true; break; }
        }
        if (already_known || *known_count == MAX_KNOWN_SOCKETS) continue;
        known[*known_count] = strdup(entry->d_name);
        if (known[*known_count] == NULL) continue;
        (*known_count)++;
        log_line(log, "observed Wayland socket: %s/%s", runtime_directory, entry->d_name);
    }
    closedir(directory);
}

static bool process_is_owned_by_current_user(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld", (long)pid);
    struct stat metadata;
    return stat(path, &metadata) == 0 && metadata.st_uid == getuid();
}

static bool process_comm(pid_t pid, char *result, size_t result_size) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/comm", (long)pid);
    FILE *file = fopen(path, "r");
    if (!file) return false;
    bool read = fgets(result, (int)result_size, file) != NULL;
    fclose(file);
    if (!read) return false;
    result[strcspn(result, "\n")] = '\0';
    return true;
}

static pid_t process_parent(pid_t pid) {
    char path[64], line[256];
    snprintf(path, sizeof(path), "/proc/%ld/status", (long)pid);
    FILE *file = fopen(path, "r");
    if (!file) return 0;
    pid_t parent = 0;
    while (fgets(line, sizeof(line), file) != NULL) {
        long parsed = 0;
        if (sscanf(line, "PPid:\t%ld", &parsed) == 1) { parent = (pid_t)parsed; break; }
    }
    fclose(file);
    return parent;
}

static bool is_descendant_of(pid_t process, pid_t ancestor) {
    for (int depth = 0; process > 1 && depth < 64; depth++) {
        if (process == ancestor) return true;
        process = process_parent(process);
    }
    return false;
}

static pid_t find_process_named(const char *name) {
    DIR *directory = opendir("/proc");
    if (!directory) return 0;
    struct dirent *entry;
    pid_t result = 0;
    while ((entry = readdir(directory)) != NULL) {
        if (!is_decimal(entry->d_name)) continue;
        pid_t pid = (pid_t)strtol(entry->d_name, NULL, 10);
        char comm[64];
        if (process_is_owned_by_current_user(pid) && process_comm(pid, comm, sizeof(comm)) && strcmp(comm, name) == 0) {
            result = pid;
            break;
        }
    }
    closedir(directory);
    return result;
}

static bool read_command_socket(pid_t pid, char *socket, size_t socket_size) {
    char path[64], arguments[8192];
    snprintf(path, sizeof(path), "/proc/%ld/cmdline", (long)pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    ssize_t length = read(fd, arguments, sizeof(arguments) - 1);
    close(fd);
    if (length <= 0) return false;
    arguments[length] = '\0';
    bool next_is_socket = false;
    for (char *argument = arguments; argument < arguments + length; argument += strlen(argument) + 1) {
        if (*argument == '\0') continue;
        if (next_is_socket) {
            if (strchr(argument, '/') == NULL && strlen(argument) < socket_size) {
                strcpy(socket, argument);
                return true;
            }
            return false;
        }
        if (strcmp(argument, "--socket") == 0) next_is_socket = true;
        else if (strncmp(argument, "--socket=", 9) == 0 && strchr(argument + 9, '/') == NULL && strlen(argument + 9) < socket_size) {
            strcpy(socket, argument + 9);
            return true;
        }
    }
    return false;
}

static pid_t find_nested_kwin_socket(pid_t plasma_session, const char *runtime_directory, char *socket, size_t socket_size) {
    DIR *directory = opendir("/proc");
    if (!directory) return 0;
    struct dirent *entry;
    pid_t result = 0;
    while ((entry = readdir(directory)) != NULL) {
        if (!is_decimal(entry->d_name)) continue;
        pid_t pid = (pid_t)strtol(entry->d_name, NULL, 10);
        char comm[64];
        if (!process_is_owned_by_current_user(pid) || !process_comm(pid, comm, sizeof(comm)) || strcmp(comm, "kwin_wayland") != 0) continue;
        if (!is_descendant_of(pid, plasma_session) || !read_command_socket(pid, socket, socket_size)) continue;
        if (!is_socket(runtime_directory, socket)) continue;
        result = pid;
        break;
    }
    closedir(directory);
    return result;
}

static char **session_environment(pid_t plasma_session, const char *socket, bool *has_dbus, bool *has_runtime) {
    char path[64], bytes[MAX_ENVIRONMENT_BYTES];
    snprintf(path, sizeof(path), "/proc/%ld/environ", (long)plasma_session);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    ssize_t length = read(fd, bytes, sizeof(bytes) - 1);
    close(fd);
    if (length < 0) return NULL;
    bytes[length] = '\0';
    size_t count = 0;
    for (ssize_t index = 0; index < length; index += (ssize_t)strlen(bytes + index) + 1) {
        if (bytes[index] != '\0' && strncmp(bytes + index, "WAYLAND_DISPLAY=", 16) != 0) count++;
    }
    char **environment = calloc(count + 2, sizeof(char *));
    if (!environment) return NULL;
    size_t output = 0;
    *has_dbus = false;
    *has_runtime = false;
    for (ssize_t index = 0; index < length; index += (ssize_t)strlen(bytes + index) + 1) {
        char *entry = bytes + index;
        if (*entry == '\0' || strncmp(entry, "WAYLAND_DISPLAY=", 16) == 0) continue;
        if (strncmp(entry, "DBUS_SESSION_BUS_ADDRESS=", 25) == 0) *has_dbus = true;
        if (strncmp(entry, "XDG_RUNTIME_DIR=", 16) == 0) *has_runtime = true;
        environment[output] = strdup(entry);
        if (!environment[output]) {
            for (size_t cleanup = 0; cleanup < output; cleanup++) free(environment[cleanup]);
            free(environment);
            return NULL;
        }
        output++;
    }
    size_t required = strlen("WAYLAND_DISPLAY=") + strlen(socket) + 1;
    environment[output] = malloc(required);
    if (!environment[output]) {
        for (size_t cleanup = 0; cleanup < output; cleanup++) free(environment[cleanup]);
        free(environment);
        return NULL;
    }
    snprintf(environment[output], required, "WAYLAND_DISPLAY=%s", socket);
    return environment;
}

static void free_environment(char **environment) {
    if (!environment) return;
    for (size_t index = 0; environment[index]; index++) free(environment[index]);
    free(environment);
}

static pid_t launch_plasmashell(FILE *log, pid_t plasma_session, const char *socket) {
    bool has_dbus = false, has_runtime = false;
    char **environment = session_environment(plasma_session, socket, &has_dbus, &has_runtime);
    if (!environment) {
        log_line(log, "plasma-wayland: unable to read plasma_session environment");
        return 0;
    }
    if (!has_dbus || !has_runtime) {
        log_line(log, "plasma-wayland: refusing fallback; plasma_session environment lacks %s%s",
                 has_dbus ? "" : "DBUS_SESSION_BUS_ADDRESS ", has_runtime ? "" : "XDG_RUNTIME_DIR");
        free_environment(environment);
        return 0;
    }
    pid_t child = fork();
    if (child < 0) {
        log_line(log, "plasma-wayland: fork failed: %s", strerror(errno));
        free_environment(environment);
        return 0;
    }
    if (child == 0) {
        char *const argv[] = {"plasmashell", "--replace", NULL};
        execve("/usr/bin/plasmashell", argv, environment);
        dprintf(STDERR_FILENO, "unable to launch plasmashell: %s\n", strerror(errno));
        _exit(127);
    }
    free_environment(environment);
    log_line(log, "plasma-wayland: started plasmashell --replace on %s (pid=%ld)", socket, (long)child);
    return child;
}

static void supervise_child(FILE *log, pid_t child) {
    managed_child = child;
    while (running) {
        int status = 0;
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            log_line(log, "plasma-wayland: managed plasmashell exited with status=%d", status);
            managed_child = 0;
            return;
        }
        if (result < 0 && errno != EINTR) {
            log_line(log, "plasma-wayland: waitpid failed: %s", strerror(errno));
            managed_child = 0;
            return;
        }
        sleep_milliseconds(POLL_MS);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    managed_child = 0;
    log_line(log, "plasma-wayland: managed plasmashell stopped with session");
}

static long parse_milliseconds(const char *value) {
    char *end = NULL;
    errno = 0;
    long milliseconds = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || milliseconds < 0 || milliseconds > 60000) return -1;
    return milliseconds;
}

int main(int argc, char **argv) {
    const char *runtime_directory = NULL, *log_path = NULL;
    long observe_milliseconds = DEFAULT_OBSERVE_MS;
    for (int index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--runtime-dir") == 0 && index + 1 < argc) runtime_directory = argv[++index];
        else if (strcmp(argv[index], "--log") == 0 && index + 1 < argc) log_path = argv[++index];
        else if (strcmp(argv[index], "--observe-ms") == 0 && index + 1 < argc) observe_milliseconds = parse_milliseconds(argv[++index]);
        else {
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
    char *known[MAX_KNOWN_SOCKETS] = {0};
    size_t known_count = 0;
    long socket_seen_at = -1;
    bool plasma_adapter_finished = false;
    log_line(log, "session supervisor started; runtime=%s observe_ms=%ld", runtime_directory, observe_milliseconds);
    for (long elapsed = 0; running && elapsed <= observe_milliseconds; elapsed += POLL_MS) {
        observe_sockets(log, runtime_directory, known, &known_count);
        if (!plasma_adapter_finished) {
            pid_t plasma_session = find_process_named("plasma_session");
            char socket[NAME_MAX + 1] = {0};
            pid_t kwin = plasma_session ? find_nested_kwin_socket(plasma_session, runtime_directory, socket, sizeof(socket)) : 0;
            if (kwin > 0) {
                if (socket_seen_at < 0) {
                    socket_seen_at = elapsed;
                    log_line(log, "plasma-wayland: detected plasma_session=%ld kwin_wayland=%ld socket=%s", (long)plasma_session, (long)kwin, socket);
                }
                if (find_process_named("plasmashell") > 0) {
                    log_line(log, "plasma-wayland: plasmashell is already running; no fallback needed");
                    plasma_adapter_finished = true;
                } else if (elapsed - socket_seen_at >= PLASMA_GRACE_MS) {
                    log_line(log, "plasma-wayland: plasmashell absent after %dms; starting fallback", PLASMA_GRACE_MS);
                    pid_t child = launch_plasmashell(log, plasma_session, socket);
                    plasma_adapter_finished = true;
                    if (child > 0) supervise_child(log, child);
                    break;
                }
            }
        }
        if (elapsed == observe_milliseconds) break;
        sleep_milliseconds(POLL_MS);
    }
    if (!plasma_adapter_finished) log_line(log, "plasma-wayland: adapter did not match during startup observation");
    log_line(log, "session supervisor observation finished; sockets=%zu", known_count);
    for (size_t index = 0; index < known_count; index++) free(known[index]);
    fclose(log);
    return 0;
}
