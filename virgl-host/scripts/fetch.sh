#!/usr/bin/env bash
set -euo pipefail

package_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sources_dir="$package_dir/sources"
work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT

virgl_version="1.3.0"
libepoxy_version="1.5.10"
angle_url="https://packages.termux.dev/apt/termux-main/pool/main/a/angle-android/angle-android_2.1.24923-f09a19ce-2_aarch64.deb"

[[ ! -e "$sources_dir" ]] || {
    echo "Refusing to overwrite existing sources: $sources_dir" >&2
    exit 1
}

for tool in ar curl sha256sum tar; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "Missing required tool: $tool" >&2
        exit 1
    }
done

fetch_archive() {
    local url="$1" sha256="$2" archive="$3"
    curl --fail --location --retry 2 --output "$archive" "$url"
    printf '%s  %s\n' "$sha256" "$archive" | sha256sum --check --status
}

mkdir -p "$sources_dir/archives" "$sources_dir/virglrenderer" "$sources_dir/libepoxy"

virgl_archive="$work_dir/virglrenderer-${virgl_version}.tar.gz"
fetch_archive \
    "https://gitlab.freedesktop.org/virgl/virglrenderer/-/archive/virglrenderer-${virgl_version}/virglrenderer-virglrenderer-${virgl_version}.tar.gz" \
    "56170f8caa1bb642a2624b649e3bcca095ec2834814e5c308efc8a85a709e4ce" \
    "$virgl_archive"
tar --extract --gzip --file "$virgl_archive" --strip-components=1 --directory "$sources_dir/virglrenderer"
printf '%s\n' "virglrenderer-${virgl_version}" > "$sources_dir/virglrenderer/.trierarch-source-version"

epoxy_archive="$work_dir/libepoxy-${libepoxy_version}.tar.gz"
fetch_archive \
    "https://github.com/anholt/libepoxy/archive/refs/tags/${libepoxy_version}.tar.gz" \
    "a7ced37f4102b745ac86d6a70a9da399cc139ff168ba6b8002b4d8d43c900c15" \
    "$epoxy_archive"
tar --extract --gzip --file "$epoxy_archive" --strip-components=1 --directory "$sources_dir/libepoxy"
printf '%s\n' "$libepoxy_version" > "$sources_dir/libepoxy/.trierarch-source-version"

angle_archive="$sources_dir/archives/angle-android-arm64.deb"
fetch_archive \
    "$angle_url" \
    "3e421cecefc8cb5ca9c57ffe1eb38f77dc315d88d99f1018ef0fdd2add6ca330" \
    "$angle_archive"

echo "Prepared unmodified VirGL sources at $sources_dir"
