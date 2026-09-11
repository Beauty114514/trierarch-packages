#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
build_dir="$project_dir/build"
dist_dir="$project_dir/dist"
sysroot_dir="${TRIERARCH_WAYLAND_IME_SYSROOT:-$project_dir/.sysroot/debian-bookworm-arm64}"
protocol=/usr/share/wayland-protocols/unstable/input-method/input-method-unstable-v1.xml

mkdir -p "$build_dir" "$dist_dir"
wayland-scanner client-header "$protocol" "$build_dir/input-method-v1-client-protocol.h"
wayland-scanner private-code "$protocol" "$build_dir/input-method-v1-client-protocol.c"
cc=${CC:-cc}
case "$cc" in
  *aarch64-linux-gnu*)
    include="$sysroot_dir/usr/include"
    library="$sysroot_dir/usr/lib/aarch64-linux-gnu"
    test -f "$include/wayland-client.h" && test -f "$library/libwayland-client.so" || {
      printf '%s\n' "Missing Debian arm64 Wayland sysroot; run scripts/fetch-debian-arm64-sysroot.sh" >&2
      exit 1
    }
    "$cc" -std=c11 -Wall -Wextra -Werror -I"$build_dir" -I"$include" \
      -L"$library" -Wl,-rpath-link,"$library" \
      "$project_dir/src/main.c" "$build_dir/input-method-v1-client-protocol.c" \
      -lwayland-client -o "$dist_dir/trierarch-wayland-ime-bridge"
    ;;
  *)
    "$cc" -std=c11 -Wall -Wextra -Werror -I"$build_dir" \
      "$project_dir/src/main.c" "$build_dir/input-method-v1-client-protocol.c" \
      $(pkg-config --cflags --libs wayland-client) -o "$dist_dir/trierarch-wayland-ime-bridge"
    ;;
esac
