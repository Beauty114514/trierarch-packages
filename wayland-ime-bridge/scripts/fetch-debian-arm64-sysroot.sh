#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
destination="${TRIERARCH_WAYLAND_IME_SYSROOT:-$project_dir/.sysroot/debian-bookworm-arm64}"
downloads="$destination/.downloads"
base=https://deb.debian.org/debian/pool/main/w/wayland

command -v curl >/dev/null || { printf '%s\n' 'curl is required.' >&2; exit 127; }
command -v dpkg-deb >/dev/null || { printf '%s\n' 'dpkg-deb is required.' >&2; exit 127; }
mkdir -p "$downloads"

for package in libwayland-dev_1.21.0-1_arm64.deb libwayland-client0_1.21.0-1_arm64.deb; do
    archive="$downloads/$package"
    if [ ! -f "$archive" ]; then
        curl --fail --location --retry 3 --output "$archive" "$base/$package"
    fi
    dpkg-deb --extract "$archive" "$destination"
done

ffi_archive="$downloads/libffi8_3.4.4-1_arm64.deb"
if [ ! -f "$ffi_archive" ]; then
    curl --fail --location --retry 3 --output "$ffi_archive" \
        https://deb.debian.org/debian/pool/main/libf/libffi/libffi8_3.4.4-1_arm64.deb
fi
dpkg-deb --extract "$ffi_archive" "$destination"

test -f "$destination/usr/include/wayland-client.h"
test -f "$destination/usr/lib/aarch64-linux-gnu/libwayland-client.so"
test -f "$destination/usr/lib/aarch64-linux-gnu/libffi.so.8"
printf '%s\n' "Prepared Debian Bookworm arm64 Wayland sysroot at $destination"
