# Trierarch Linux compatibility layers

This package contains small, opt-in compatibility layers for Linux software
running inside Trierarch-managed PRoot, chroot, and DroidSpaces environments.
They bridge narrowly-defined host restrictions; they are not container
runtimes, desktop-environment forks, or replacements for system libraries.

## libudev monitor fallback

`src/libtrierarch-udev-compat.c` is an `LD_PRELOAD` shim for a specific Linux
container restriction: Android denies `NETLINK_KOBJECT_UEVENT` to the app
domain. As a result, `udev_monitor_new_from_netlink()` can fail with `EACCES`.

Some KWin 6.7 versions treat the monitor as mandatory and dereference the
failed result before compositor initialization. This shim first calls the real
libudev function. If it succeeds, it does not alter the real monitor at all.
Only when creation fails does it return an idle, eventfd-backed monitor. KWin
can then enumerate devices available at startup but receives no runtime device
hotplug events.

The shim does **not** provide a GPU, grant Android permissions, alter SELinux,
or emulate `/dev/dri`. It only makes an unavailable *hotplug notification*
optional.

Trierarch enables the fallback automatically for profiles by default. It can
be disabled per profile when a guest needs to observe real udev hotplug events:

```toml
[compat]
udev_monitor = "off"
```

`auto` is the default. It uses the real monitor whenever it is available; it
does not export a permanent shell-profile setting. For an isolated manual test,
inject it only for the target launch session:

```bash
LD_PRELOAD=/path/to/libtrierarch-udev-compat.so \
  dbus-run-session -- kwin_wayland --virtual --width 1080 --height 2244
```

Set `TRIERARCH_UDEV_COMPAT_DEBUG=1` to print a single diagnostic when the
fallback is selected.

## Build

The artifact is a Linux guest shared library, not an Android NDK/bionic
library. It must be compiled for the target container ABI.

For the first Arch PRoot test, compile natively inside that container:

```bash
cd /path/to/trierarch-packages/compat
bash scripts/build-linux.sh
```

This writes:

```text
dist/linux/aarch64/libtrierarch-udev-compat.so
```

On a development computer, select an appropriate Linux cross compiler, for
example:

```bash
CC=aarch64-linux-gnu-gcc bash scripts/build-linux.sh
```

The Android app build cross-compiles this library with `aarch64-linux-gnu-gcc`,
packages it as an asset, then deploys it at launch: PRoot copies it under the
rootfs, chroot copies it after elevation, and DroidSpaces bind-mounts then
copies it into guest `/tmp` before setting `LD_PRELOAD`. A glibc build is
appropriate for the current Arch, Debian, and Ubuntu rootfses; a future musl
rootfs needs a separately compiled artifact.
