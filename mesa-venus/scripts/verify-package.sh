#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 1 ]; then
    printf 'usage: %s /path/to/vulkan-virtio-*.pkg.tar.*\n' "$0" >&2
    exit 64
fi

package=$1
test -f "$package"

contents=$(bsdtar -tf "$package")
printf '%s\n' "$contents" | grep -qx 'usr/lib/libvulkan_virtio.so'
printf '%s\n' "$contents" | grep -Eq '^usr/share/vulkan/icd.d/virtio_icd(\\.aarch64)?\\.json$'

printf '%s\n' 'vulkan-virtio package contents verified'
