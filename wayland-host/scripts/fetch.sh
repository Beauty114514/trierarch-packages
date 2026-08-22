#!/usr/bin/env bash
set -euo pipefail

package_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="$package_dir/sources"
tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

wayland_repo="https://gitlab.freedesktop.org/wayland/wayland.git"
wayland_commit="87cc8a8728a923fc57938faa81ba0e74f34ecdc7"
protocols_repo="https://gitlab.freedesktop.org/wayland/wayland-protocols.git"
protocols_commit="ee78491a237eaff9389a0ccf8680521d074407d3"
libffi_version="3.4.6"

fetch_source() {
    local name="$1" repo="$2" commit="$3"
    local checkout="$tmp_dir/$name"

    echo "Fetching $name at $commit"
    git clone --filter=blob:none "$repo" "$checkout"
    git -C "$checkout" checkout --detach "$commit"
    rm -rf "$checkout/.git"

    mkdir -p "$source_dir"
    rm -rf "$source_dir/$name"
    mv "$checkout" "$source_dir/$name"
    printf '%s\n' "$commit" > "$source_dir/$name/.trierarch-source-commit"
}

command -v git >/dev/null 2>&1 || {
    echo "Missing required tool: git" >&2
    exit 1
}

fetch_source wayland "$wayland_repo" "$wayland_commit"
fetch_source wayland-protocols "$protocols_repo" "$protocols_commit"
command -v curl >/dev/null 2>&1 || { echo "Missing required tool: curl" >&2; exit 1; }
libffi_dir="$source_dir/libffi"
if [[ ! -d "$libffi_dir" ]]; then
    echo "Fetching libffi ${libffi_version}"
    archive="$tmp_dir/libffi-${libffi_version}.tar.gz"
    curl -fL "https://github.com/libffi/libffi/releases/download/v${libffi_version}/libffi-${libffi_version}.tar.gz" -o "$archive"
    mkdir -p "$source_dir"
    tar -xzf "$archive" -C "$source_dir"
    mv "$source_dir/libffi-${libffi_version}" "$libffi_dir"
    printf '%s\n' "$libffi_version" > "$libffi_dir/.trierarch-source-version"
fi
echo "Prepared Wayland sources under $source_dir"
