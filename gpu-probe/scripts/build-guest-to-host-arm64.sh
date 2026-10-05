#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
packages_dir=$(CDPATH= cd -- "$project_dir/.." && pwd)
sysroot_dir="$packages_dir/wayland-ime-bridge/.sysroot/debian-bookworm-arm64"
include_dir="$sysroot_dir/usr/include"
library_dir="$sysroot_dir/usr/lib/aarch64-linux-gnu"
output_path=${TRIERARCH_GPU_PROBE_OUTPUT:-/tmp/trierarch-guest-to-host-probe}

test -f "$include_dir/gbm.h" &&
    test -f "$include_dir/libdrm/drm_fourcc.h" &&
    test -f "$library_dir/libgbm.so" && {
    :
} || {
    printf '%s\n' 'Missing Debian arm64 GBM/DRM sysroot.' >&2
    exit 1
}

aarch64-linux-gnu-gcc -O2 -Wall -Wextra -Werror -std=c11 \
    -I"$project_dir" -I"$include_dir" -I"$include_dir/libdrm" \
    "$project_dir/guest/guest_to_host.c" \
    -L"$library_dir" -Wl,-rpath-link,"$library_dir" \
    -Wl,-rpath-link,"$sysroot_dir/lib/aarch64-linux-gnu" \
    -lgbm -ldrm -o "$output_path"
printf 'Built %s\n' "$output_path"
