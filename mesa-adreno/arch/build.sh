#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
package_root=${TRIERARCH_MESA_BUILD_DIR:-"$script_dir/../.build"}
pkgbuild_repo=${MESA_PKGBUILD_REPO:-https://github.com/lfdevs/archlinuxarm-PKGBUILDs.git}
pkgbuild_branch=${MESA_PKGBUILD_BRANCH:-freedreno}
checkout_dir="$package_root/archlinuxarm-PKGBUILDs"
mesa_dir="$checkout_dir/extra/mesa"

if [[ "$(id -u)" -eq 0 ]]; then
    printf '%s\n' 'Run this script as the guest build user, not root.' >&2
    exit 1
fi

command -v git >/dev/null || {
    printf '%s\n' 'git is required.' >&2
    exit 1
}
command -v makepkg >/dev/null || {
    printf '%s\n' 'makepkg is required.' >&2
    exit 1
}

mkdir -p "$package_root"
if [[ ! -d "$checkout_dir/.git" ]]; then
    git clone --depth=1 --branch "$pkgbuild_branch" "$pkgbuild_repo" "$checkout_dir"
else
    git -C "$checkout_dir" fetch --depth=1 origin "$pkgbuild_branch"
    git -C "$checkout_dir" reset --hard "origin/$pkgbuild_branch"
fi

if [[ ! -f "$mesa_dir/PKGBUILD" ]]; then
    printf 'Mesa PKGBUILD was not found at %s\n' "$mesa_dir" >&2
    exit 1
fi

cd "$mesa_dir"
exec "$script_dir/makepkg.sh" "$@"
