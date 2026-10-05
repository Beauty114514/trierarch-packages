#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
profile="$root_dir/targets/arch-aarch64.toml"

usage() {
    cat <<'EOF'
Usage: configure-arch-aarch64.sh <source-dir> <build-dir> <private-prefix>

Creates a fresh Meson configuration for the locked Adreno Mesa reference
profile.  It does not run ninja or install any files.
EOF
}

profile_options() {
    awk '
        /^meson_options = \[/ { collecting = 1; next }
        collecting && /^]/ { exit }
        collecting {
            gsub(/^[[:space:]]*"|",?[[:space:]]*$/, "")
            if (length($0)) print
        }
    ' "$profile"
}

if [[ $# -ne 3 ]]; then
    usage >&2
    exit 64
fi

source_dir=$1
build_dir=$2
private_prefix=$3

[[ $(uname -m) == "aarch64" ]] || {
    printf '%s\n' 'mesa-adreno: Arch aarch64 configuration must run on a native aarch64 guest.' >&2
    exit 1
}

command -v meson >/dev/null || {
    printf '%s\n' 'mesa-adreno: meson is required; install the target distribution build dependencies first.' >&2
    exit 1
}

[[ -f $source_dir/meson.build ]] || {
    printf 'mesa-adreno: not a Mesa source directory: %s\n' "$source_dir" >&2
    exit 1
}

[[ ! -e $build_dir ]] || {
    printf 'mesa-adreno: refusing to reuse existing build directory: %s\n' "$build_dir" >&2
    exit 1
}

mapfile -t options < <(profile_options)
meson setup "$build_dir" "$source_dir" \
    --prefix "$private_prefix" \
    --libdir lib \
    "${options[@]}"

printf 'mesa-adreno: configured %s\n' "$build_dir"
