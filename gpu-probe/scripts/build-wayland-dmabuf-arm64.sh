#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
packages_dir=$(CDPATH= cd -- "$project_dir/.." && pwd)
sysroot_dir="$packages_dir/wayland-ime-bridge/.sysroot/debian-bookworm-arm64"
protocol_dir="$packages_dir/wayland-host/sources/wayland-protocols/stable"
include_dir="$sysroot_dir/usr/include"
library_dir="$sysroot_dir/usr/lib/aarch64-linux-gnu"
output_path=${TRIERARCH_GPU_PROBE_OUTPUT:-/tmp/trierarch-wayland-dmabuf-probe}

test -f "$include_dir/wayland-client.h" &&
    test -f "$library_dir/libwayland-client.so" &&
    test -f "$library_dir/libgbm.so" || {
    printf '%s\n' 'Missing Debian arm64 Wayland/GBM sysroot.' >&2
    exit 1
}

build_dir=$(mktemp -d)
trap 'rm -r -- "$build_dir"' EXIT HUP INT TERM
wayland-scanner client-header "$protocol_dir/linux-dmabuf/linux-dmabuf-v1.xml" \
    "$build_dir/linux-dmabuf-v1-client-protocol.h"
wayland-scanner private-code "$protocol_dir/linux-dmabuf/linux-dmabuf-v1.xml" \
    "$build_dir/linux-dmabuf-v1-protocol.c"
wayland-scanner client-header "$protocol_dir/xdg-shell/xdg-shell.xml" \
    "$build_dir/xdg-shell-client-protocol.h"
wayland-scanner private-code "$protocol_dir/xdg-shell/xdg-shell.xml" \
    "$build_dir/xdg-shell-protocol.c"

aarch64-linux-gnu-gcc -O2 -Wall -Wextra -Werror -std=c11 \
    -I"$build_dir" -I"$include_dir" -I"$include_dir/libdrm" \
    "$project_dir/guest/wayland_dmabuf.c" \
    "$build_dir/linux-dmabuf-v1-protocol.c" "$build_dir/xdg-shell-protocol.c" \
    -L"$library_dir" -Wl,-rpath-link,"$library_dir" \
    -Wl,-rpath-link,"$sysroot_dir/lib/aarch64-linux-gnu" \
    -lwayland-client -lgbm -ldrm -o "$output_path"
printf 'Built %s\n' "$output_path"
