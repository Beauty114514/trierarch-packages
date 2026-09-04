/*
 * Trierarch Linux compatibility layer: optional libudev monitor fallback.
 *
 * Android can deny NETLINK_KOBJECT_UEVENT to an untrusted app's container.
 * libudev then returns NULL from udev_monitor_new_from_netlink(). KWin 6.7
 * creates a QSocketNotifier from that monitor before it initializes any
 * compositor backend. This preload library leaves real monitors untouched and
 * substitutes an idle eventfd-backed monitor only when the real call fails.
 */
#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

struct udev;
struct udev_device;
struct udev_monitor;

typedef struct udev_monitor *(*new_monitor_fn)(struct udev *, const char *);
typedef struct udev_monitor *(*unref_monitor_fn)(struct udev_monitor *);
typedef int (*monitor_filter_fn)(struct udev_monitor *, const char *, const char *);
typedef int (*monitor_enable_fn)(struct udev_monitor *);
typedef int (*monitor_fd_fn)(struct udev_monitor *);
typedef struct udev_device *(*monitor_receive_fn)(struct udev_monitor *);

struct compat_monitor {
    struct udev_monitor *public_handle;
    int fd;
    struct compat_monitor *next;
};

static pthread_mutex_t monitor_lock = PTHREAD_MUTEX_INITIALIZER;
static struct compat_monitor *compat_monitors;
static pthread_once_t resolver_once = PTHREAD_ONCE_INIT;

static new_monitor_fn real_new_monitor;
static unref_monitor_fn real_unref_monitor;
static monitor_filter_fn real_filter;
static monitor_enable_fn real_enable;
static monitor_fd_fn real_get_fd;
static monitor_receive_fn real_receive;

static void resolve_symbols(void)
{
    real_new_monitor = (new_monitor_fn)dlsym(RTLD_NEXT, "udev_monitor_new_from_netlink");
    real_unref_monitor = (unref_monitor_fn)dlsym(RTLD_NEXT, "udev_monitor_unref");
    real_filter = (monitor_filter_fn)dlsym(RTLD_NEXT, "udev_monitor_filter_add_match_subsystem_devtype");
    real_enable = (monitor_enable_fn)dlsym(RTLD_NEXT, "udev_monitor_enable_receiving");
    real_get_fd = (monitor_fd_fn)dlsym(RTLD_NEXT, "udev_monitor_get_fd");
    real_receive = (monitor_receive_fn)dlsym(RTLD_NEXT, "udev_monitor_receive_device");
}

static bool debug_enabled(void)
{
    const char *value = getenv("TRIERARCH_UDEV_COMPAT_DEBUG");
    return value && strcmp(value, "0") != 0;
}

static void debug_log(const char *format, ...)
{
    if (!debug_enabled()) {
        return;
    }

    va_list args;
    va_start(args, format);
    fputs("trierarch-udev-compat: ", stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
}

static struct compat_monitor *find_compat_monitor(struct udev_monitor *monitor)
{
    struct compat_monitor *current;

    pthread_mutex_lock(&monitor_lock);
    for (current = compat_monitors; current; current = current->next) {
        if (current->public_handle == monitor) {
            break;
        }
    }
    pthread_mutex_unlock(&monitor_lock);
    return current;
}

static struct compat_monitor *remove_compat_monitor(struct udev_monitor *monitor)
{
    struct compat_monitor **cursor;
    struct compat_monitor *found = NULL;

    pthread_mutex_lock(&monitor_lock);
    for (cursor = &compat_monitors; *cursor; cursor = &(*cursor)->next) {
        if ((*cursor)->public_handle == monitor) {
            found = *cursor;
            *cursor = found->next;
            break;
        }
    }
    pthread_mutex_unlock(&monitor_lock);
    return found;
}

struct udev_monitor *udev_monitor_new_from_netlink(struct udev *udev, const char *name)
{
    pthread_once(&resolver_once, resolve_symbols);
    if (!real_new_monitor) {
        errno = ENOSYS;
        return NULL;
    }

    struct udev_monitor *monitor = real_new_monitor(udev, name);
    if (monitor) {
        return monitor;
    }

    const int original_errno = errno;
    struct compat_monitor *fallback = calloc(1, sizeof(*fallback));
    if (!fallback) {
        errno = ENOMEM;
        return NULL;
    }

    fallback->fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (fallback->fd < 0) {
        free(fallback);
        return NULL;
    }

    fallback->public_handle = (struct udev_monitor *)fallback;
    pthread_mutex_lock(&monitor_lock);
    fallback->next = compat_monitors;
    compat_monitors = fallback;
    pthread_mutex_unlock(&monitor_lock);

    debug_log("libudev monitor '%s' unavailable (%s); using idle fallback", name ? name : "", strerror(original_errno));
    return fallback->public_handle;
}

struct udev_monitor *udev_monitor_unref(struct udev_monitor *monitor)
{
    pthread_once(&resolver_once, resolve_symbols);
    struct compat_monitor *fallback = remove_compat_monitor(monitor);
    if (fallback) {
        close(fallback->fd);
        free(fallback);
        return NULL;
    }
    return real_unref_monitor ? real_unref_monitor(monitor) : NULL;
}

int udev_monitor_filter_add_match_subsystem_devtype(struct udev_monitor *monitor, const char *subsystem, const char *devtype)
{
    pthread_once(&resolver_once, resolve_symbols);
    if (find_compat_monitor(monitor)) {
        return 0;
    }
    return real_filter ? real_filter(monitor, subsystem, devtype) : -1;
}

int udev_monitor_enable_receiving(struct udev_monitor *monitor)
{
    pthread_once(&resolver_once, resolve_symbols);
    if (find_compat_monitor(monitor)) {
        return 0;
    }
    return real_enable ? real_enable(monitor) : -1;
}

int udev_monitor_get_fd(struct udev_monitor *monitor)
{
    pthread_once(&resolver_once, resolve_symbols);
    struct compat_monitor *fallback = find_compat_monitor(monitor);
    if (fallback) {
        return fallback->fd;
    }
    return real_get_fd ? real_get_fd(monitor) : -1;
}

struct udev_device *udev_monitor_receive_device(struct udev_monitor *monitor)
{
    pthread_once(&resolver_once, resolve_symbols);
    if (find_compat_monitor(monitor)) {
        errno = EAGAIN;
        return NULL;
    }
    return real_receive ? real_receive(monitor) : NULL;
}
