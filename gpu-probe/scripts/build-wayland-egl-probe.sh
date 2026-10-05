#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
protocol_dir=${WAYLAND_PROTOCOLS_DIR:-/usr/share/wayland-protocols}
output_path=${TRIERARCH_WAYLAND_EGL_PROBE_OUTPUT:-/tmp/trierarch-wayland-egl-probe}

command -v wayland-scanner >/dev/null || {
    printf '%s\n' 'wayland-scanner is required.' >&2
    exit 1
}
test -f "$protocol_dir/stable/xdg-shell/xdg-shell.xml" || {
    printf '%s\n' 'xdg-shell.xml is required; set WAYLAND_PROTOCOLS_DIR if needed.' >&2
    exit 1
}

build_dir=$(mktemp -d)
trap 'rm -rf -- "$build_dir"' EXIT HUP INT TERM
wayland-scanner client-header "$protocol_dir/stable/xdg-shell/xdg-shell.xml" \
    "$build_dir/xdg-shell-client-protocol.h"
wayland-scanner private-code "$protocol_dir/stable/xdg-shell/xdg-shell.xml" \
    "$build_dir/xdg-shell-protocol.c"

${CC:-cc} -O2 -Wall -Wextra -Werror -std=c11 \
    -I"$build_dir" "$project_dir/guest/wayland_egl_probe.c" \
    "$build_dir/xdg-shell-protocol.c" \
    $(pkg-config --cflags --libs wayland-client wayland-egl egl glesv2) \
    -o "$output_path"
printf 'Built %s\n' "$output_path"
