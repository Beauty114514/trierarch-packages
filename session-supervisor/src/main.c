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

enum {
    DEFAULT_OBSERVE_MS = 10000,
    DEFAULT_GRACE_MS = 8000,
    DISCOVERY_POLL_MS = 200,
    STEADY_POLL_MS = 1000,
    SHELL_SCAN_MS = 1000,
    POLL_MS = 1000,
    MAX_KNOWN_SOCKETS = 32,
    MAX_ENVIRONMENT_BYTES = 131072,
    MAX_FALLBACK_ATTEMPTS = 5,
    MAX_BACKOFF_MS = 30000,
};
static const char *const DROIDSPACES_BINARY = "/data/local/Droidspaces/bin/droidspaces";

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

static bool is_decimal(const char *value);

static bool systemd_user_available(void) {
    if (access("/run/systemd/system", F_OK) != 0) return false;
    DIR *directory = opendir("/run/user");
    if (!directory) return false;
    bool available = false;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (!is_decimal(entry->d_name)) continue;
        char path[PATH_MAX];
        int length = snprintf(path, sizeof(path), "/run/user/%s/systemd/private", entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(path)) continue;
        struct stat metadata;
        if (lstat(path, &metadata) == 0 && S_ISSOCK(metadata.st_mode)) {
            available = true;
            break;
        }
    }
    closedir(directory);
    return available;
}

static long fallback_backoff_ms(int attempts) {
    long delay = 1000;
    for (int index = 1; index < attempts && delay < MAX_BACKOFF_MS; index++) {
        delay *= 2;
        if (delay > MAX_BACKOFF_MS) delay = MAX_BACKOFF_MS;
    }
    return delay;
}

static void write_status(const char *path, const char *state, pid_t plasma_session,
        pid_t kwin, pid_t shell, pid_t fallback, int attempts, bool systemd,
        const char *socket) {
    if (!path) return;
    char temporary[PATH_MAX];
    int length = snprintf(temporary, sizeof(temporary), "%s.%ld.tmp", path, (long)getpid());
    if (length < 0 || (size_t)length >= sizeof(temporary)) return;
    FILE *file = fopen(temporary, "w");
    if (!file) return;
    fprintf(file, "{\n");
    fprintf(file, "  \"state\": \"%s\",\n", state);
    fprintf(file, "  \"plasma_session\": %ld,\n", (long)plasma_session);
    fprintf(file, "  \"kwin\": %ld,\n", (long)kwin);
    fprintf(file, "  \"socket\": \"%s\",\n", socket ? socket : "");
    fprintf(file, "  \"plasmashell\": %ld,\n", (long)shell);
    fprintf(file, "  \"fallback_pid\": %ld,\n", (long)fallback);
    fprintf(file, "  \"fallback_attempts\": %d,\n", attempts);
    fprintf(file, "  \"systemd_user\": %s\n", systemd ? "true" : "false");
    fprintf(file, "}\n");
    if (fclose(file) != 0 || rename(temporary, path) != 0) unlink(temporary);
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

static bool process_is_zombie(pid_t pid) {
    char path[64], line[256];
    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    FILE *file = fopen(path, "r");
    if (!file) return true;
    bool zombie = true;
    if (fgets(line, sizeof(line), file)) {
        char *end = strrchr(line, ')');
        if (end && end[1] == ' ') zombie = end[2] == 'Z';
    }
    fclose(file);
    return zombie;
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

static bool process_start_time(pid_t pid, unsigned long long *start_time) {
    char path[64], line[512];
    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    FILE *file = fopen(path, "r");
    if (!file || !fgets(line, sizeof(line), file)) {
        if (file) fclose(file);
        return false;
    }
    fclose(file);
    char *fields = strrchr(line, ')');
    if (!fields || fields[1] != ' ') return false;
    char *state_end = strchr(fields + 2, ' ');
    if (!state_end) return false;
    char *cursor = state_end + 1;
    while (*cursor == ' ') cursor++;
    for (int index = 1; index < 20; index++) {
        char *end = NULL;
        unsigned long long value = strtoull(cursor, &end, 10);
        if (!end || end == cursor) return false;
        if (index == 19) {
            *start_time = value;
            return true;
        }
        cursor = end;
        while (*cursor == ' ') cursor++;
    }
    return false;
}

static bool process_matches(pid_t pid, unsigned long long start_time) {
    if (pid <= 0 || start_time == 0) return false;
    unsigned long long current = 0;
    return process_start_time(pid, &current) && current == start_time;
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
        if (process_is_owned_by_current_user(pid) && process_comm(pid, comm, sizeof(comm)) &&
                strcmp(comm, name) == 0 && !process_is_zombie(pid)) {
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

static bool owner_is_alive(pid_t owner_pid, unsigned long long owner_start_time) {
    char path[64], line[512];
    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)owner_pid);
    FILE *file = fopen(path, "r");
    if (!file || !fgets(line, sizeof(line), file)) {
        if (file) fclose(file);
        return false;
    }
    fclose(file);
    char *fields = strrchr(line, ')');
    if (!fields) return false;
    fields += 2;
    char *cursor = fields;
    for (int index = 0; index < 20; index++) {
        char *end = NULL;
        unsigned long long value = strtoull(cursor, &end, 10);
        if (index == 19) return value == owner_start_time;
        if (!end || *end == '\0') return false;
        cursor = end + 1;
        while (*cursor == ' ') cursor++;
    }
    return false;
}

static int stop_droidspaces(const char *container) {
    pid_t child = fork();
    if (child < 0) return 1;
    if (child == 0) {
        execl(DROIDSPACES_BINARY, "droidspaces", "--name", container, "stop", (char *)NULL);
        dprintf(STDERR_FILENO, "unable to execute %s: %s\n", DROIDSPACES_BINARY, strerror(errno));
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    return status;
}

static int watch_owner(pid_t owner_pid, unsigned long long owner_start_time, pid_t session_pgid,
                       const char *stop_container) {
    while (owner_is_alive(owner_pid, owner_start_time)) sleep_milliseconds(POLL_MS);

    if (stop_container) {
        dprintf(STDERR_FILENO, "owner %ld is gone; stopping DroidSpaces container %s\n",
                (long)owner_pid, stop_container);
        return stop_droidspaces(stop_container);
    }

    /* The Android App process disappeared or its PID was reused. The
     * supervisor is independent from the shell, so it can terminate the whole
     * chroot session even after Magisk su reparents that shell to magiskd. */
    if (session_pgid > 1) {
        kill(-session_pgid, SIGTERM);
        sleep_milliseconds(250);
        kill(-session_pgid, SIGKILL);
    }
    return 0;
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

static long parse_milliseconds(const char *value) {
    char *end = NULL;
    errno = 0;
    long milliseconds = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || milliseconds < 0 || milliseconds > 60000) return -1;
    return milliseconds;
}

int main(int argc, char **argv) {
    const char *runtime_directory = NULL, *log_path = NULL, *status_path = NULL;
    const char *fallback_mode = "auto";
    long observe_milliseconds = DEFAULT_OBSERVE_MS;
    long grace_milliseconds = DEFAULT_GRACE_MS;
    bool dry_run = false;
    pid_t owner_pid = 0;
    unsigned long long owner_start_time = 0;
    pid_t session_pgid = 0;
    const char *stop_container = NULL;
    for (int index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--runtime-dir") == 0 && index + 1 < argc) runtime_directory = argv[++index];
        else if (strcmp(argv[index], "--log") == 0 && index + 1 < argc) log_path = argv[++index];
        else if (strcmp(argv[index], "--status-file") == 0 && index + 1 < argc) status_path = argv[++index];
        else if (strcmp(argv[index], "--observe-ms") == 0 && index + 1 < argc) observe_milliseconds = parse_milliseconds(argv[++index]);
        else if (strcmp(argv[index], "--grace-ms") == 0 && index + 1 < argc) grace_milliseconds = parse_milliseconds(argv[++index]);
        else if (strcmp(argv[index], "--fallback") == 0 && index + 1 < argc) fallback_mode = argv[++index];
        else if (strcmp(argv[index], "--dry-run") == 0) dry_run = true;
        else if (strcmp(argv[index], "--owner-pid") == 0 && index + 1 < argc) owner_pid = (pid_t)strtol(argv[++index], NULL, 10);
        else if (strcmp(argv[index], "--owner-start-time") == 0 && index + 1 < argc) owner_start_time = strtoull(argv[++index], NULL, 10);
        else if (strcmp(argv[index], "--pgid") == 0 && index + 1 < argc) session_pgid = (pid_t)strtol(argv[++index], NULL, 10);
        else if (strcmp(argv[index], "--stop-container") == 0 && index + 1 < argc) stop_container = argv[++index];
        else {
            fprintf(stderr, "usage: %s --runtime-dir DIRECTORY --log FILE [--status-file FILE] "
                    "[--observe-ms 0..60000] [--grace-ms 0..60000] "
                    "[--fallback auto|always|off] [--dry-run] | "
                    "--owner-pid PID --owner-start-time TICKS (--pgid PGID | --stop-container NAME)\n", argv[0]);
            return 2;
        }
    }
    if (owner_pid > 0 || owner_start_time > 0 || session_pgid > 0 || stop_container) {
        if (owner_pid <= 1 || owner_start_time == 0 ||
                (session_pgid > 1) == (stop_container != NULL)) {
            fprintf(stderr, "--owner-pid, --owner-start-time and exactly one of --pgid/--stop-container are required\n");
            return 2;
        }
        if (stop_container && (!*stop_container || strpbrk(stop_container, "/ \t\n"))) {
            fprintf(stderr, "invalid DroidSpaces container name\n");
            return 2;
        }
        return watch_owner(owner_pid, owner_start_time, session_pgid, stop_container);
    }
    if (!runtime_directory || !log_path || observe_milliseconds < 0 ||
            grace_milliseconds < 0 ||
            (strcmp(fallback_mode, "auto") != 0 &&
             strcmp(fallback_mode, "always") != 0 &&
             strcmp(fallback_mode, "off") != 0)) {
        fprintf(stderr, "runtime directory, log file, valid durations, and fallback mode are required\n");
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
    bool discovered = false;
    bool wait_logged = false;
    bool shell_logged = false;
    bool exhausted_logged = false;
    bool systemd_user = false;
    long socket_seen_at = -1;
    long next_retry_at = 0;
    long last_shell_scan = -SHELL_SCAN_MS;
    long last_status_write = -5000;
    int fallback_attempts = 0;
    pid_t fallback_pid = 0;
    pid_t plasma_session = 0;
    pid_t kwin = 0;
    pid_t shell = 0;
    unsigned long long plasma_start = 0, kwin_start = 0, shell_start = 0;
    char socket[NAME_MAX + 1] = {0};
    const char *state = "DISCOVER";
    const char *last_status_state = NULL;
    long elapsed = 0;
    log_line(log, "session supervisor started; runtime=%s observe_ms=%ld grace_ms=%ld fallback=%s dry_run=%d",
            runtime_directory, observe_milliseconds, grace_milliseconds, fallback_mode, dry_run ? 1 : 0);
    while (running) {
        if (!discovered) {
            observe_sockets(log, runtime_directory, known, &known_count);
            plasma_session = find_process_named("plasma_session");
            if (plasma_session > 0) {
                unsigned long long candidate_start = 0;
                if (process_start_time(plasma_session, &candidate_start)) {
                    kwin = find_nested_kwin_socket(plasma_session, runtime_directory,
                            socket, sizeof(socket));
                    if (kwin > 0 && process_start_time(kwin, &kwin_start)) {
                        plasma_start = candidate_start;
                        discovered = true;
                        socket_seen_at = elapsed;
                        systemd_user = systemd_user_available();
                        last_shell_scan = elapsed - SHELL_SCAN_MS;
                        state = "WAIT_SHELL";
                        log_line(log, "plasma-wayland: detected plasma_session=%ld kwin_wayland=%ld socket=%s",
                                (long)plasma_session, (long)kwin, socket);
                    } else {
                        kwin = 0;
                    }
                }
            }
            if (!discovered && elapsed >= observe_milliseconds) {
                log_line(log, "plasma-wayland: adapter did not match during startup observation");
                break;
            }
        } else {
            if (!process_matches(plasma_session, plasma_start)) {
                log_line(log, "plasma-wayland: plasma_session exited");
                break;
            }
            if (!process_matches(kwin, kwin_start)) {
                log_line(log, "plasma-wayland: kwin_wayland exited");
                break;
            }
            if (fallback_pid > 0) {
                int status = 0;
                pid_t reaped = waitpid(fallback_pid, &status, WNOHANG);
                if (reaped == fallback_pid) {
                    log_line(log, "plasma-wayland: fallback plasmashell exited with status=%d", status);
                    fallback_pid = 0;
                    managed_child = 0;
                    last_shell_scan = elapsed - SHELL_SCAN_MS;
                    if (shell <= 0 && fallback_attempts < MAX_FALLBACK_ATTEMPTS)
                        next_retry_at = elapsed + fallback_backoff_ms(fallback_attempts);
                }
            }
            if (shell > 0 && !process_matches(shell, shell_start)) {
                shell = 0;
                shell_start = 0;
                last_shell_scan = elapsed - SHELL_SCAN_MS;
            }
            if (shell == 0 && elapsed - last_shell_scan >= SHELL_SCAN_MS) {
                last_shell_scan = elapsed;
                pid_t found = find_process_named("plasmashell");
                if (found > 0 && process_start_time(found, &shell_start)) shell = found;
            }
            if (shell > 0) {
                if (!shell_logged) {
                    shell_logged = true;
                    state = "SHELL_OK";
                    log_line(log, "plasma-wayland: plasmashell detected; leaving lifecycle to startplasma");
                }
                fallback_attempts = 0;
                next_retry_at = 0;
                wait_logged = false;
                exhausted_logged = false;
            } else {
                shell_logged = false;
                bool allow_fallback = strcmp(fallback_mode, "off") != 0 &&
                        (strcmp(fallback_mode, "always") == 0 || !systemd_user);
                if (systemd_user && strcmp(fallback_mode, "always") != 0) {
                    state = "WAIT_STARTPLASMA";
                } else if (allow_fallback && elapsed - socket_seen_at >= grace_milliseconds) {
                    if (!wait_logged) {
                        wait_logged = true;
                        log_line(log, "plasma-wayland: plasmashell absent after %ldms; starting fallback",
                                grace_milliseconds);
                    }
                    if (fallback_attempts >= MAX_FALLBACK_ATTEMPTS && !exhausted_logged) {
                        exhausted_logged = true;
                        state = "FALLBACK_EXHAUSTED";
                        log_line(log, "plasma-wayland: fallback attempts exhausted");
                    }
                    if (fallback_pid == 0 && fallback_attempts < MAX_FALLBACK_ATTEMPTS &&
                            elapsed >= next_retry_at) {
                        if (dry_run) {
                            fallback_attempts++;
                            next_retry_at = elapsed + fallback_backoff_ms(fallback_attempts);
                            state = "FALLBACK_DRY_RUN";
                            log_line(log, "plasma-wayland: dry-run would launch plasmashell --replace on %s (attempt %d)",
                                    socket, fallback_attempts);
                        } else {
                            pid_t child = launch_plasmashell(log, plasma_session, socket);
                            fallback_attempts++;
                            if (child > 0) {
                                fallback_pid = child;
                                managed_child = child;
                                state = "SUPERVISE_FALLBACK";
                            } else {
                                next_retry_at = elapsed + fallback_backoff_ms(fallback_attempts);
                                state = "FALLBACK_FAILED";
                            }
                        }
                    }
                } else {
                    state = "WAIT_SHELL";
                }
            }
        }
        bool status_due = !last_status_state || strcmp(state, last_status_state) != 0 ||
                elapsed - last_status_write >= 5000;
        if (status_due) {
            write_status(status_path, state, plasma_session, kwin, shell, fallback_pid,
                    fallback_attempts, systemd_user, socket);
            last_status_state = state;
            last_status_write = elapsed;
        }
        long poll_ms = discovered ? STEADY_POLL_MS : DISCOVERY_POLL_MS;
        elapsed += poll_ms;
        if (running) sleep_milliseconds(poll_ms);
    }
    if (fallback_pid > 0) {
        kill(fallback_pid, SIGTERM);
        int status = 0;
        while (waitpid(fallback_pid, &status, 0) < 0 && errno == EINTR) {}
        fallback_pid = 0;
        managed_child = 0;
    }
    write_status(status_path, state, plasma_session, kwin, shell, fallback_pid,
            fallback_attempts, systemd_user, socket);
    log_line(log, "session supervisor stopped; sockets=%zu", known_count);
    for (size_t index = 0; index < known_count; index++) free(known[index]);
    fclose(log);
    return 0;
}
