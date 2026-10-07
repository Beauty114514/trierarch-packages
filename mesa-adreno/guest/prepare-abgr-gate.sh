#!/usr/bin/env bash
set -euo pipefail

source_dir=${1:-"$HOME/trierarch-adreno-mesa"}
patch_file=${2:-/tmp/trierarch-wayland-host/adreno-bridge/0001-wayland-abgr-format-gate.patch}
remote=https://github.com/lfdevs/mesa-for-android-container.git
tag=mesa-26.3.0-devel-20260824
revision=98f3d6229d61452cef80f8563af7c56ae599dc14

if [[ -e $source_dir && ! -d $source_dir/.git ]]; then
    printf 'refusing non-Git source path: %s\n' "$source_dir" >&2
    exit 64
fi

if [[ ! -d $source_dir/.git ]]; then
    git clone --branch "$tag" --single-branch "$remote" "$source_dir"
fi

actual=$(git -C "$source_dir" rev-parse HEAD)
if [[ $actual != "$revision" ]]; then
    printf 'unexpected Mesa revision: %s (expected %s)\n' "$actual" "$revision" >&2
    exit 65
fi

[[ -f $patch_file ]] || {
    printf 'missing Trierarch patch: %s\n' "$patch_file" >&2
    exit 66
}

if git -C "$source_dir" apply --reverse --check "$patch_file" >/dev/null 2>&1; then
    printf 'Mesa ABGR gate already applied: %s\n' "$source_dir"
else
    git -C "$source_dir" apply --check "$patch_file"
    git -C "$source_dir" apply "$patch_file"
    printf 'Mesa ABGR gate applied: %s\n' "$source_dir"
fi

printf '%s\n' 'Source is prepared only. Configure and build it in a separate step.'
