#!/usr/bin/env bash
set -euo pipefail

package_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
output_dir="$package_dir/dist/android/arm64-v8a"
cc="${CC:-cc}"

mkdir -p "$output_dir"
"$cc" ${CFLAGS:--O2 -fPIC -Wall -Wextra} -shared \
    -o "$output_dir/libgbm-shim.so" "$package_dir/gbm_shim.c" -ldl
echo "Android GBM shim written to $output_dir"
