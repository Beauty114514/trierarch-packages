#!/usr/bin/env bash
set -euo pipefail

package_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sources_dir="$package_dir/sources"
proot_url="https://github.com/termux/proot.git"
proot_commit="a89b3732ec6ae1db674510f0843b2f3db54d0a2f"

if [[ -e "$sources_dir" ]]; then
    echo "Refusing to overwrite existing sources: $sources_dir" >&2
    exit 1
fi

for tool in git curl sha256sum tar; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "Missing required tool: $tool" >&2
        exit 1
    }
done

work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT

git clone --filter=blob:none --no-checkout "$proot_url" "$work_dir/proot"
git -C "$work_dir/proot" checkout --detach "$proot_commit"

mkdir -p "$sources_dir/proot" "$sources_dir/talloc" "$sources_dir/libandroid-shmem"
git -C "$work_dir/proot" archive --format=tar "$proot_commit" \
    | tar --extract --directory="$sources_dir/proot"

fetch_archive() {
    local url="$1" sha256="$2" destination="$3"
    local archive="$work_dir/$(basename "$destination").tar.gz"

    curl --fail --location --retry 2 --output "$archive" "$url"
    printf '%s  %s\n' "$sha256" "$archive" | sha256sum --check --status
    tar --extract --file="$archive" --strip-components=1 --directory="$destination"
}

fetch_archive \
    "https://www.samba.org/ftp/talloc/talloc-2.4.3.tar.gz" \
    "dc46c40b9f46bb34dd97fe41f548b0e8b247b77a918576733c528e83abd854dd" \
    "$sources_dir/talloc"
fetch_archive \
    "https://github.com/termux/libandroid-shmem/archive/refs/tags/v0.7.tar.gz" \
    "1e5ff8459bc0a8c229dd8a94b27d119987e09ef3414331c2b5ebfff20b98e867" \
    "$sources_dir/libandroid-shmem"

echo "Prepared unmodified sources at $sources_dir"
