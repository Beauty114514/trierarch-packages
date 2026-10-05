#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
packages_dir=$(CDPATH= cd -- "$project_dir/.." && pwd)
sysroot_dir="$packages_dir/wayland-ime-bridge/.sysroot/debian-bookworm-arm64"
protocol_dir="$packages_dir/wayland-host/sources/wayland-protocols/stable"
include_dir="$sysroot_dir/usr/include"
library_dir="$sysroot_dir/usr/lib/aarch64-linux-gnu"
output_path=${TRIERARCH_GPU_PROBE_OUTPUT:-/tmp/trierarch-host-ahb-wayland-probe}

test -f "$include_dir/wayland-client.h" &&
    test -f "$library_dir/libwayland-client.so" || {
    printf '%s\n' 'Missing Debian arm64 Wayland client sysroot.' >&2
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
    -I"$build_dir" -I"$project_dir" -I"$include_dir" \
    "$project_dir/guest/host_ahb_wayland.c" "$project_dir/guest/host_ahb_channel.c" \
    "$build_dir/linux-dmabuf-v1-protocol.c" "$build_dir/xdg-shell-protocol.c" \
    -L"$library_dir" -Wl,-rpath-link,"$library_dir" \
    -Wl,-rpath-link,"$sysroot_dir/lib/aarch64-linux-gnu" \
    -lwayland-client -o "$output_path"
printf 'Built %s\n' "$output_path"
