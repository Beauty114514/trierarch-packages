#!/usr/bin/env bash
set -euo pipefail

usage() {
    echo "usage: $0 --source DIR --build DIR [--jobs N]" >&2
    exit 2
}

source_dir=
build_dir=
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
while (($#)); do
    case "$1" in
        --source) source_dir=${2:?}; shift 2 ;;
        --build) build_dir=${2:?}; shift 2 ;;
        --jobs|-j) jobs=${2:?}; shift 2 ;;
        *) usage ;;
    esac
done
[[ -n "$source_dir" && -n "$build_dir" ]] || usage
source_dir=$(realpath "$source_dir")
build_dir=$(realpath -m "$build_dir")
patch_file=$(dirname "$(realpath "$0")")/patches/0001-wayland-render-node.patch
[[ -f "$source_dir/src/backends/wayland/wayland_backend.cpp" ]] || {
    echo "not a KWin source tree: $source_dir" >&2
    exit 1
}
[[ -f "$patch_file" ]] || { echo "missing patch: $patch_file" >&2; exit 1; }

if patch -d "$source_dir" -p1 --dry-run --silent < "$patch_file"; then
    patch -d "$source_dir" -p1 --silent < "$patch_file"
elif grep -q 'TRIERARCH_NESTED_RENDER_NODE' "$source_dir/src/backends/wayland/wayland_backend.cpp"; then
    echo "patch already applied to $source_dir"
else
    echo "patch does not apply; rebase patches/0001-wayland-render-node.patch for this KWin version" >&2
    exit 1
fi

cmake_args=(
    -S "$source_dir"
    -B "$build_dir"
    -DCMAKE_BUILD_TYPE=Release
    -DBUILD_TESTING=OFF
    -DKDE_INSTALL_LIBEXECDIR=lib
)
if [[ ! -f "$build_dir/CMakeCache.txt" ]]; then
    cmake "${cmake_args[@]}"
fi
cmake --build "$build_dir" --target kwin -j "$jobs"

lib=$(find "$build_dir/bin" -maxdepth 1 -name 'libkwin.so.*' -type f -print -quit)
[[ -n "$lib" ]] || { echo "libkwin was not produced" >&2; exit 1; }
echo "built: $lib"
