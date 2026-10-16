#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
build_root=${TRIERARCH_MESA_DEBIAN_BUILD_DIR:-"$script_dir/../.debian-build"}
source_dir=${MESA_DEBIAN_SOURCE_DIR:-}
jobs=${MESA_BUILD_JOBS:-$(nproc)}
source_spec=${MESA_DEBIAN_SOURCE_SPEC:-mesa}
expected_version=${MESA_DEBIAN_MESA_VERSION:-26.1.6}
expected_source_package_version=${MESA_DEBIAN_SOURCE_PACKAGE_VERSION:-26.1.6-1}

if [[ "$(id -u)" -eq 0 ]]; then
    printf '%s\n' 'Run dpkg-buildpackage as the guest build user, not root.' >&2
    exit 1
fi

for command_name in apt-get dpkg-buildpackage patch; do
    command -v "$command_name" >/dev/null || {
        printf 'Required command not found: %s\n' "$command_name" >&2
        exit 1
    }
done

mkdir -p "$build_root"

if [[ -z "$source_dir" ]]; then
    if ! command -v dpkg-source >/dev/null; then
        printf '%s\n' 'dpkg-source is required when preparing Mesa with apt-get source.' >&2
        exit 1
    fi
    printf '%s\n' 'Downloading the Debian/Kali Mesa source package...'
    (
        cd "$build_root"
        apt-get source "$source_spec"
    )
    source_dir=$(find "$build_root" -mindepth 1 -maxdepth 1 -type d -name 'mesa-*' -print -quit)
fi

if [[ -z "$source_dir" || ! -f "$source_dir/debian/rules" ]]; then
    printf 'A Debian Mesa source tree with debian/rules was not found: %s\n' "${source_dir:-<empty>}" >&2
    printf '%s\n' 'Install/enable a matching deb-src entry, or set MESA_DEBIAN_SOURCE_DIR.' >&2
    exit 1
fi

actual_version=$(tr -d '[:space:]' < "$source_dir/VERSION" 2>/dev/null || true)
if [[ -n "$expected_version" && "$actual_version" != "$expected_version" ]]; then
    printf 'Mesa source version mismatch: expected %s, got %s\n' \
        "$expected_version" "${actual_version:-unknown}" >&2
    printf '%s\n' 'Set MESA_DEBIAN_MESA_VERSION only after reviewing the patch queue.' >&2
    exit 1
fi

actual_source_package_version=$(dpkg-parsechangelog -l"$source_dir/debian/changelog" -S Version)
if [[ -n "$expected_source_package_version" &&
        "$actual_source_package_version" != "$expected_source_package_version" ]]; then
    printf 'Debian Mesa source package mismatch: expected %s, got %s\n' \
        "$expected_source_package_version" "${actual_source_package_version:-unknown}" >&2
    printf '%s\n' 'Set MESA_DEBIAN_SOURCE_PACKAGE_VERSION only after reviewing the patch queue.' >&2
    exit 1
fi

patch_dir="$script_dir/patches"
if [[ -d "$patch_dir" ]]; then
    while IFS= read -r patch_file; do
        printf 'Applying %s\n' "$patch_file"
        patch --forward --batch -d "$source_dir" -p1 < "$patch_file"
    done < <(find "$patch_dir" -maxdepth 1 -type f -name '*.patch' -print | sort)
fi

printf 'Building Debian packages from %s\n' "$source_dir"
cd "$source_dir"
exec dpkg-buildpackage -b -nc -us -uc -j"$jobs" "$@"
