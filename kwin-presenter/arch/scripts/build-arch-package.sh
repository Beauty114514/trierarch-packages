#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
arch_dir="$(cd "$script_dir/.." && pwd)"
project_dir="$(cd "$arch_dir/.." && pwd)"
packages_dir="$(cd "$project_dir/.." && pwd)"
version='6.7.5'
work_dir="$HOME/.cache/trierarch/kwin-presenter"
stage_dir="$work_dir/stage"
overlay_dir="$work_dir/overlay/kwin-$version/src/backends/trierarch"
package_dir="$work_dir/packages"

[[ "$(id -u)" -ne 0 ]] || {
    printf '%s\n' 'makepkg must run as an unprivileged user' >&2
    exit 1
}
[[ "$(uname -m)" == 'aarch64' ]] || {
    printf '%s\n' 'this package must be built natively on Arch Linux ARM (aarch64)' >&2
    exit 1
}
command -v makepkg >/dev/null

rm -rf "$stage_dir" "$work_dir/overlay"
mkdir -p "$stage_dir" "$overlay_dir" "$package_dir"
cp "$arch_dir/PKGBUILD" "$stage_dir/PKGBUILD"
cp "$arch_dir/patches/0001-add-trierarch-presenter-probe.patch" "$stage_dir/kwin.patch"
cp -a "$arch_dir/overlay/src/backends/trierarch/." "$overlay_dir/"
cp "$packages_dir/presenter-bridge/trierarch_presenter_protocol.h" "$overlay_dir/"
tar -chf "$stage_dir/trierarch-overlay.tar" -C "$work_dir/overlay" "kwin-$version/src/backends/trierarch"

(
    cd "$stage_dir"
    PKGDEST="$package_dir" makepkg -C -f -s --clean
)

printf '%s\n' "Built package artifacts in $package_dir"
