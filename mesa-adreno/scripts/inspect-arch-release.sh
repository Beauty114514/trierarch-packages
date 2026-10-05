#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=source-lock.sh
source "$script_dir/source-lock.sh"

require_entry() {
    local package=$1 entry=$2
    if ! bsdtar -tf "$package" | grep -Fqx "$entry"; then
        printf 'mesa-adreno: missing %s in %s\n' "$entry" "$(basename -- "$package")" >&2
        exit 1
    fi
}

archive=${1:?"usage: inspect-arch-release.sh <release-archive.tar>"}
mesa_adreno_verify_lock

expected=$(mesa_adreno_lock_value release_artifact_sha256)
actual=$(sha256sum "$archive" | awk '{print $1}')
if [[ $actual != "$expected" ]]; then
    printf 'mesa-adreno: archive SHA-256 mismatch: got %s, expected %s\n' "$actual" "$expected" >&2
    exit 1
fi

work_dir=$(mktemp -d "${TMPDIR:-/tmp}/trierarch-mesa-release.XXXXXX")
trap 'rm -rf "$work_dir"' EXIT
tar -xf "$archive" -C "$work_dir"

mesa_package=$(find "$work_dir" -maxdepth 1 -name 'mesa-[0-9]*.pkg.tar.*' -print -quit)
vulkan_package=$(find "$work_dir" -maxdepth 1 -name 'vulkan-freedreno-*.pkg.tar.*' -print -quit)
[[ -n $mesa_package && -n $vulkan_package ]] || {
    printf '%s\n' 'mesa-adreno: expected mesa and vulkan-freedreno packages were not found.' >&2
    exit 1
}

for entry in \
    usr/lib/libEGL_mesa.so.0 \
    usr/lib/libGLX_mesa.so.0 \
    usr/lib/libgbm.so.1 \
    usr/lib/dri/kgsl_dri.so \
    usr/lib/dri/libdril_dri.so \
    usr/lib/gbm/dri_gbm.so \
    usr/share/glvnd/egl_vendor.d/50_mesa.json; do
    require_entry "$mesa_package" "$entry"
done

for entry in \
    usr/lib/libvulkan_freedreno.so \
    usr/share/vulkan/icd.d/freedreno_icd.aarch64.json; do
    require_entry "$vulkan_package" "$entry"
done

printf '%s\n' 'mesa-adreno: Arch aarch64 release matches the private-bundle contract.'
