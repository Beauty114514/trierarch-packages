#!/usr/bin/env bash
set -euo pipefail

package_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out_dir="${1:-$package_dir/build/host-tests/protocol-generated}"
protocols_dir="${WAYLAND_PROTOCOLS_DIR:-/usr/share/wayland-protocols}"
scanner="${WAYLAND_SCANNER:-wayland-scanner}"

command -v "$scanner" >/dev/null || {
    echo "wayland-scanner is required" >&2
    exit 1
}
[[ -d "$protocols_dir" ]] || {
    echo "wayland-protocols not found at $protocols_dir" >&2
    exit 1
}

rm -rf "$out_dir"
mkdir -p "$out_dir"

generate() {
    local xml="$1"
    local name="$2"
    [[ -f "$xml" ]] || { echo "missing protocol XML: $xml" >&2; exit 1; }
    "$scanner" server-header "$xml" "$out_dir/$name-server-protocol.h"
    "$scanner" private-code "$xml" "$out_dir/$name-protocol.c"
}

generate "$protocols_dir/stable/xdg-shell/xdg-shell.xml" xdg-shell
generate "$protocols_dir/staging/single-pixel-buffer/single-pixel-buffer-v1.xml" single-pixel-buffer-v1
generate "$protocols_dir/stable/viewporter/viewporter.xml" viewporter
generate "$protocols_dir/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml" pointer-constraints
generate "$protocols_dir/stable/presentation-time/presentation-time.xml" presentation-time
generate "$protocols_dir/stable/linux-dmabuf/linux-dmabuf-v1.xml" linux-dmabuf-v1
generate "$protocols_dir/unstable/xdg-output/xdg-output-unstable-v1.xml" xdg-output-unstable-v1
generate "$protocols_dir/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml" xdg-decoration-unstable-v1
generate "$protocols_dir/staging/fractional-scale/fractional-scale-v1.xml" fractional-scale-v1
generate "$protocols_dir/unstable/relative-pointer/relative-pointer-unstable-v1.xml" relative-pointer-unstable-v1
generate "$package_dir/protocol/android-wlegl.xml" android-wlegl
generate "$package_dir/protocol/wlr-layer-shell-unstable-v1.xml" wlr-layer-shell-unstable-v1
generate "$protocols_dir/unstable/linux-explicit-synchronization/linux-explicit-synchronization-unstable-v1.xml" linux-explicit-synchronization-unstable-v1
