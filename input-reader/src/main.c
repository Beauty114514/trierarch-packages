// Root-only physical keyboard bridge for Trierarch.
//
// Android's input policy consumes Meta/Super before an application can see it.
// This helper runs as root, exclusively grabs external keyboard evdev devices,
// and streams their key events to the app over stdout. The app forwards those
// raw evdev codes to the Wayland host, where they have the same meaning as the
// codes used by wl_keyboard.
//
// The helper intentionally does not touch built-in input devices. It only
// grabs keyboards whose kernel phys path identifies an external USB device.
#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MAX_DEVICES 8
#define FRAME_MAGIC 0x54524b42u /* "TRKB" */
#define POLL_MS 250
#define OWNER_CHECK_MS 1000
#define RESCAN_MS 30000

struct key_frame {
    uint32_t magic;
    uint32_t key;
    uint32_t value;
    uint32_t seconds;
    uint32_t microseconds;
};

struct device {
    char path[PATH_MAX];
    int fd;
    pthread_t thread;
    bool active;
    bool verbose;
};

static struct device devices[MAX_DEVICES];
static pthread_mutex_t devices_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t pressed_lock = PTHREAD_MUTEX_INITIALIZER;
static bool pressed_keys[KEY_MAX + 1];
static volatile sig_atomic_t running = 1;
static pid_t owner_pid = -1;
static bool verbose;

static void log_line(const char *format, ...) {
    if (!verbose) return;
    va_list arguments;
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    fputc('\n', stderr);
    fflush(stderr);
}

static void on_signal(int signal_number) {
    (void)signal_number;
    running = 0;
}

static bool has_key(int fd, unsigned int key) {
    unsigned char bits[(KEY_MAX + 1 + 7) / 8];
    memset(bits, 0, sizeof(bits));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0) return false;
    return (bits[key / 8] & (1u << (key % 8))) != 0;
}

static bool external_keyboard(int fd, char *name, size_t name_size) {
    unsigned char event_bits[(EV_MAX + 1 + 7) / 8];
    char phys[256] = {0};

    if (ioctl(fd, EVIOCGBIT(0, sizeof(event_bits)), event_bits) < 0) return false;
    if (!(event_bits[EV_KEY / 8] & (1u << (EV_KEY % 8)))) return false;
    if (!has_key(fd, KEY_Q) || !has_key(fd, KEY_A)) return false;

    if (ioctl(fd, EVIOCGNAME(name_size), name) < 0) name[0] = '\0';
    if (ioctl(fd, EVIOCGPHYS(sizeof(phys)), phys) < 0) phys[0] = '\0';

    // Do not grab phone buttons, touch panels, or virtual input devices.
    if (strstr(name, "gpio-keys") || strstr(name, "pmic") || strstr(name, "touch") ||
        strstr(name, "hall") || strstr(name, "uinput"))
        return false;

    // A USB keyboard's phys starts with "usb-". Bluetooth keyboards use a
    // Bluetooth address and are also safe to grab when they report KEY_Q/A.
    return strncmp(phys, "usb-", 4) == 0 || strchr(phys, ':') != NULL;
}

static bool write_frame(const struct input_event *event) {
    struct key_frame frame = {
        .magic = FRAME_MAGIC,
        .key = (uint32_t)event->code,
        .value = (uint32_t)event->value,
        .seconds = (uint32_t)event->time.tv_sec,
        .microseconds = (uint32_t)event->time.tv_usec,
    };
    const unsigned char *data = (const unsigned char *)&frame;
    size_t remaining = sizeof(frame);
    while (remaining > 0) {
        ssize_t written = write(STDOUT_FILENO, data, remaining);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (written == 0) return false;
        data += written;
        remaining -= (size_t)written;
    }
    return true;
}

static void *read_device(void *argument) {
    struct device *device = argument;
    int fd = device->fd;

    if (ioctl(fd, EVIOCGRAB, 1) < 0) {
        log_line("EVIOCGRAB failed for %s: %s", device->path, strerror(errno));
        close(fd);
        pthread_mutex_lock(&devices_lock);
        device->fd = -1;
        device->active = false;
        pthread_mutex_unlock(&devices_lock);
        return NULL;
    }
    log_line("grabbed %s", device->path);

    while (running) {
        struct pollfd poll_info = {.fd = fd, .events = POLLIN};
        int poll_result = poll(&poll_info, 1, POLL_MS);
        if (poll_result < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (poll_result == 0) continue;
        if (poll_info.revents & (POLLERR | POLLHUP | POLLNVAL)) break;

        struct input_event event;
        ssize_t length = read(fd, &event, sizeof(event));
        if (length == 0) break;
        if (length < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if ((size_t)length < sizeof(event)) continue;
        if (event.type != EV_KEY) continue;
        if (!write_frame(&event)) break;
        pthread_mutex_lock(&pressed_lock);
        pressed_keys[event.code & KEY_MAX] = event.value != 0;
        pthread_mutex_unlock(&pressed_lock);
    }

    ioctl(fd, EVIOCGRAB, 0);
    close(fd);
    pthread_mutex_lock(&devices_lock);
    device->fd = -1;
    device->active = false;
    pthread_mutex_unlock(&devices_lock);
    log_line("released %s", device->path);
    return NULL;
}

static bool already_active(const char *path) {
    for (size_t index = 0; index < MAX_DEVICES; index++) {
        if (devices[index].active && strcmp(devices[index].path, path) == 0) return true;
    }
    return false;
}

static void scan_devices(void) {
    DIR *directory = opendir("/dev/input");
    if (!directory) return;

    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL && running) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;

        char path[PATH_MAX];
        int length = snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(path)) continue;

        pthread_mutex_lock(&devices_lock);
        bool duplicate = already_active(path);
        pthread_mutex_unlock(&devices_lock);
        if (duplicate) continue;

        int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;

        char name[256] = {0};
        if (!external_keyboard(fd, name, sizeof(name))) {
            close(fd);
            continue;
        }

        pthread_mutex_lock(&devices_lock);
        struct device *slot = NULL;
        for (size_t index = 0; index < MAX_DEVICES; index++) {
            if (!devices[index].active && devices[index].fd < 0) {
                slot = &devices[index];
                break;
            }
        }
        if (!slot) {
            pthread_mutex_unlock(&devices_lock);
            close(fd);
            continue;
        }
        snprintf(slot->path, sizeof(slot->path), "%s", path);
        slot->fd = fd;
        slot->active = true;
        slot->verbose = verbose;
        log_line("found external keyboard %s (%s)", path, name);
        int result = pthread_create(&slot->thread, NULL, read_device, slot);
        if (result != 0) {
            log_line("pthread_create failed for %s: %s", path, strerror(result));
            slot->fd = -1;
            slot->active = false;
            close(fd);
        } else {
            // A reader may finish when the keyboard is unplugged. Detach it so
            // its slot can be reused on hotplug without leaking a joinable
            // thread; process exit closes any remaining grabbed descriptors.
            pthread_detach(slot->thread);
        }
        pthread_mutex_unlock(&devices_lock);
    }
    closedir(directory);
}

static void release_pressed_keys(void) {
    struct input_event event;
    memset(&event, 0, sizeof(event));
    event.type = EV_KEY;
    event.value = 0;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    event.time.tv_sec = now.tv_sec;
    event.time.tv_usec = now.tv_nsec / 1000L;

    pthread_mutex_lock(&pressed_lock);
    for (unsigned int code = 0; code <= KEY_MAX; code++) {
        if (!pressed_keys[code]) continue;
        event.code = (uint16_t)code;
        if (!write_frame(&event)) break;
        pressed_keys[code] = false;
    }
    pthread_mutex_unlock(&pressed_lock);
}

static bool control_stdin_open(void) {
    struct pollfd poll_info = {.fd = STDIN_FILENO, .events = POLLIN};
    int result = poll(&poll_info, 1, 0);
    if (result < 0) return errno == EINTR;
    if (result == 0) return true;
    if (poll_info.revents & (POLLERR | POLLHUP | POLLNVAL)) return false;
    if (poll_info.revents & POLLIN) {
        char byte;
        ssize_t length = read(STDIN_FILENO, &byte, 1);
        if (length == 0) return false;
        if (length < 0 && errno != EAGAIN && errno != EINTR) return false;
    }
    return true;
}

static bool owner_alive(void) {
    if (owner_pid <= 0) return true;
    if (kill(owner_pid, 0) == 0) return true;
    return errno == EPERM;
}

static int64_t monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int open_input_watch(void) {
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) {
        log_line("inotify_init1 failed: %s", strerror(errno));
        return -1;
    }
    if (inotify_add_watch(fd, "/dev/input",
            IN_CREATE | IN_DELETE | IN_ATTRIB | IN_MOVED_FROM | IN_MOVED_TO) < 0) {
        log_line("inotify_add_watch(/dev/input) failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static void drain_input_watch(int fd) {
    char buffer[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    while (read(fd, buffer, sizeof(buffer)) > 0) {}
}

static void usage(const char *program) {
    fprintf(stderr, "usage: %s [--owner-pid PID] [--verbose]\n", program);
}

int main(int argc, char **argv) {
    for (int index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--owner-pid") == 0 && index + 1 < argc) {
            owner_pid = (pid_t)strtol(argv[++index], NULL, 10);
        } else if (strcmp(argv[index], "--verbose") == 0) {
            verbose = true;
        } else if (strcmp(argv[index], "--help") == 0 || strcmp(argv[index], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    for (size_t index = 0; index < MAX_DEVICES; index++) {
        devices[index].fd = -1;
        devices[index].active = false;
    }

    // Opening every /dev/input/event* node once per second closes the
    // non-keyboard ones again immediately; each evdev_release calls
    // synchronize_rcu() and a few of those per scan are enough to keep the
    // RCU worker busy all the time.  Watch the directory instead and keep a
    // slow safety rescan for devices that appear without an inotify event.
    int watch_fd = open_input_watch();
    int64_t next_rescan_ms = 0;
    while (running && owner_alive() && control_stdin_open()) {
        int64_t now = monotonic_ms();
        if (now >= next_rescan_ms) {
            scan_devices();
            next_rescan_ms = now + RESCAN_MS;
        }
        if (watch_fd >= 0) {
            int64_t until_rescan_ms = next_rescan_ms - monotonic_ms();
            int timeout_ms = OWNER_CHECK_MS;
            if (until_rescan_ms < timeout_ms) timeout_ms = (int)until_rescan_ms;
            if (timeout_ms < 0) timeout_ms = 0;
            struct pollfd watch = {.fd = watch_fd, .events = POLLIN};
            int result = poll(&watch, 1, timeout_ms);
            if (result > 0 && (watch.revents & POLLIN)) {
                drain_input_watch(watch_fd);
                scan_devices();
                next_rescan_ms = monotonic_ms() + RESCAN_MS;
            } else if (result < 0 && errno != EINTR) {
                log_line("inotify poll failed: %s; using periodic rescan",
                        strerror(errno));
                close(watch_fd);
                watch_fd = -1;
            }
        } else {
            struct timespec delay = {
                .tv_sec = 0, .tv_nsec = OWNER_CHECK_MS * 1000000L,
            };
            while (nanosleep(&delay, &delay) != 0 && errno == EINTR && running) {}
        }
    }
    if (watch_fd >= 0) close(watch_fd);

    running = 0;
    // Readers notice this within one poll interval and release EVIOCGRAB.
    // Detached readers are never joined here; process exit closes any remaining
    // descriptors, which also releases their kernel grabs.
    struct timespec settle = {.tv_sec = 0, .tv_nsec = POLL_MS * 1000000L};
    nanosleep(&settle, NULL);
    release_pressed_keys();
    return 0;
}
