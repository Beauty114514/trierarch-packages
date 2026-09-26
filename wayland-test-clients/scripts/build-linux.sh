#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
build_dir="$project_dir/build"
dist_dir="$project_dir/dist"
sysroot_dir="${TRIERARCH_WAYLAND_TEST_SYSROOT:-$project_dir/../wayland-ime-bridge/.sysroot/debian-bookworm-arm64}"
protocol=/usr/share/wayland-protocols/stable/linux-dmabuf/linux-dmabuf-v1.xml

mkdir -p "$build_dir" "$dist_dir"
wayland-scanner client-header "$protocol" "$build_dir/linux-dmabuf-v1-client-protocol.h"
wayland-scanner private-code "$protocol" "$build_dir/linux-dmabuf-v1-client-protocol.c"
cc=${CC:-cc}
case "$cc" in
  *aarch64-linux-gnu*)
    include="$sysroot_dir/usr/include"
    library="$sysroot_dir/usr/lib/aarch64-linux-gnu"
    runtime_library="$sysroot_dir/lib/aarch64-linux-gnu"
    test -f "$include/wayland-client.h" && test -f "$library/libwayland-client.so" || {
      printf '%s\n' "Missing arm64 Wayland sysroot; prepare wayland-ime-bridge first." >&2
      exit 1
    }
    "$cc" -std=c11 -Wall -Wextra -Werror -I"$include" \
      -L"$library" -Wl,-rpath-link,"$library" -Wl,-rpath-link,"$runtime_library" \
      "$project_dir/src/surface_lifecycle_probe.c" \
      -lwayland-client -o "$dist_dir/trierarch-surface-lifecycle-probe"
    test -f "$include/gbm.h" && test -f "$library/libgbm.so" || {
      printf '%s\n' "Missing GBM development files; run scripts/fetch-debian-arm64-sysroot.sh" >&2
      exit 1
    }
    "$cc" -std=c11 -Wall -Wextra -Werror -I"$build_dir" -I"$include" -I"$include/libdrm" \
      -L"$library" -Wl,-rpath-link,"$library" -Wl,-rpath-link,"$runtime_library" \
      "$project_dir/src/gbm_export_probe.c" \
      -lgbm -ldrm -lwayland-server -lexpat -o "$dist_dir/trierarch-gbm-export-probe"
    "$cc" -std=c11 -Wall -Wextra -Werror -I"$build_dir" -I"$include" -I"$include/libdrm" \
      -L"$library" -Wl,-rpath-link,"$library" -Wl,-rpath-link,"$runtime_library" \
      "$project_dir/src/gbm_dmabuf_lifecycle_probe.c" \
      "$build_dir/linux-dmabuf-v1-client-protocol.c" \
      -lgbm -ldrm -lwayland-client -o "$dist_dir/trierarch-gbm-dmabuf-lifecycle-probe"
    ;;
  *)
    "$cc" -std=c11 -Wall -Wextra -Werror \
      "$project_dir/src/surface_lifecycle_probe.c" \
      $(pkg-config --cflags --libs wayland-client) \
      -o "$dist_dir/trierarch-surface-lifecycle-probe"
    "$cc" -std=c11 -Wall -Wextra -Werror \
      "$project_dir/src/gbm_export_probe.c" \
      $(pkg-config --cflags --libs gbm libdrm) \
      -o "$dist_dir/trierarch-gbm-export-probe"
    "$cc" -std=c11 -Wall -Wextra -Werror -I"$build_dir" \
      "$project_dir/src/gbm_dmabuf_lifecycle_probe.c" \
      "$build_dir/linux-dmabuf-v1-client-protocol.c" \
      $(pkg-config --cflags --libs gbm libdrm wayland-client) \
      -o "$dist_dir/trierarch-gbm-dmabuf-lifecycle-probe"
    ;;
esac
