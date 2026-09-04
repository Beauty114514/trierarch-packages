#!/usr/bin/env bash
set -euo pipefail

package_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_file="$package_dir/src/libtrierarch-udev-compat.c"
compiler="${CC:-gcc}"

command -v "$compiler" >/dev/null 2>&1 || {
    echo "C compiler not found: $compiler" >&2
    exit 1
}

target_machine="$("$compiler" -dumpmachine 2>/dev/null || uname -m)"
case "$target_machine" in
    aarch64* | arm64*) target_arch="aarch64" ;;
    x86_64*) target_arch="x86_64" ;;
    *) target_arch="$target_machine" ;;
esac
out_dir="$package_dir/dist/linux/$target_arch"

mkdir -p "$out_dir"
"$compiler" \
    -shared -fPIC -O2 -Wall -Wextra -Werror \
    -o "$out_dir/libtrierarch-udev-compat.so" \
    "$source_file" \
    -ldl -pthread

echo "Linux guest compatibility library written to $out_dir/libtrierarch-udev-compat.so"
