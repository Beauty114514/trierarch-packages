#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
package_root=${TRIERARCH_MESA_BUILD_DIR:-"$script_dir/../.build"}
default_pkgbuild_repo=https://github.com/lfdevs/archlinuxarm-PKGBUILDs.git
default_pkgbuild_commit=8a29bd2e748463663852eec750b4591a36728dae
pkgbuild_repo=${MESA_PKGBUILD_REPO:-"$default_pkgbuild_repo"}
pkgbuild_branch=${MESA_PKGBUILD_BRANCH:-freedreno}
if [[ "$pkgbuild_repo" == "$default_pkgbuild_repo" && -z "${MESA_PKGBUILD_COMMIT+x}" ]]; then
    pkgbuild_commit=$default_pkgbuild_commit
else
    pkgbuild_commit=${MESA_PKGBUILD_COMMIT:-}
fi
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
    if [[ -z "$pkgbuild_commit" ]]; then
        git -C "$checkout_dir" reset --hard "origin/$pkgbuild_branch"
    fi
fi

if [[ -n "$pkgbuild_commit" ]]; then
    if ! git -C "$checkout_dir" cat-file -e "$pkgbuild_commit^{commit}" 2>/dev/null; then
        git -C "$checkout_dir" fetch --depth=1 origin "$pkgbuild_commit"
    fi
    git -C "$checkout_dir" checkout --detach "$pkgbuild_commit"
fi

if [[ ! -f "$mesa_dir/PKGBUILD" ]]; then
    printf 'Mesa PKGBUILD was not found at %s\n' "$mesa_dir" >&2
    exit 1
fi

cd "$mesa_dir"
exec "$script_dir/makepkg.sh" "$@"
