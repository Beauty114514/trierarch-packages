#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
    printf '%s\n' "usage: $0 /path/to/kwin-6.7.5" >&2
    exit 2
fi

source_dir=$1
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
arch_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
project_dir=$(CDPATH= cd -- "$arch_dir/.." && pwd)
packages_dir=$(CDPATH= cd -- "$project_dir/.." && pwd)
overlay_dir="$arch_dir/overlay/src/backends/trierarch"
protocol_header="$packages_dir/presenter-bridge/trierarch_presenter_protocol.h"
patch_file="$arch_dir/patches/0001-add-trierarch-presenter-probe.patch"

test -f "$source_dir/src/main_wayland.cpp"
test -f "$source_dir/src/backends/CMakeLists.txt"
test -f "$protocol_header"
test -d "$overlay_dir"

if [ -e "$source_dir/src/backends/trierarch" ]; then
    printf '%s\n' "refusing to overwrite existing trierarch backend overlay" >&2
    exit 1
fi

cp -R "$overlay_dir" "$source_dir/src/backends/trierarch"
cp "$protocol_header" "$source_dir/src/backends/trierarch/"
if ! patch -d "$source_dir" -p1 --forward --batch < "$patch_file"; then
    rm -rf "$source_dir/src/backends/trierarch"
    printf '%s\n' "KWin source did not match the 6.7.5 probe patch; overlay removed." >&2
    exit 1
fi

printf '%s\n' "Prepared KWin source with Trierarch presenter probe."
