#!/usr/bin/env sh
set -eu

guest_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
probe_dir=$(dirname -- "$guest_dir")
build_dir="$guest_dir/build/presenter-buffer"
mkdir -p "$build_dir"

wayland-scanner client-header \
  "$probe_dir/../wayland-host/protocol/trierarch-adreno-presenter-v1.xml" \
  "$build_dir/trierarch-adreno-presenter-v1-client-protocol.h"
wayland-scanner private-code \
  "$probe_dir/../wayland-host/protocol/trierarch-adreno-presenter-v1.xml" \
  "$build_dir/trierarch-adreno-presenter-v1-protocol.c"
cc -O2 -Wall -Wextra -std=c11 -I"$build_dir" \
  "$guest_dir/presenter_buffer_probe.c" \
  "$build_dir/trierarch-adreno-presenter-v1-protocol.c" \
  $(pkg-config --cflags --libs wayland-client egl glesv2) \
  -o "$build_dir/presenter-buffer-probe"
printf 'Built %s\n' "$build_dir/presenter-buffer-probe"
