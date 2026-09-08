#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
build_dir="$project_dir/build"
dist_dir="$project_dir/dist"
protocol=/usr/share/wayland-protocols/unstable/input-method/input-method-unstable-v1.xml

mkdir -p "$build_dir" "$dist_dir"
wayland-scanner client-header "$protocol" "$build_dir/input-method-v1-client-protocol.h"
wayland-scanner private-code "$protocol" "$build_dir/input-method-v1-client-protocol.c"
${CC:-cc} -std=c11 -Wall -Wextra -Werror \
    -I"$build_dir" \
    "$project_dir/src/main.c" "$build_dir/input-method-v1-client-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$dist_dir/trierarch-inputmethod-probe"
