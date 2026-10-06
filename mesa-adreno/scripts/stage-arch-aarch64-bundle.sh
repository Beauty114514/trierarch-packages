#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)

usage() {
    cat <<'EOF'
Usage: stage-arch-aarch64-bundle.sh <build-dir> <bundle-id> <stage-dir>

Installs an already compiled Mesa build into a disposable DESTDIR staging tree
and writes the Trierarch bundle manifest. It never writes /opt or changes the
guest's system Mesa installation.
EOF
}

toml_string() {
    local value=$1
    value=${value//\\/\\\\}
    value=${value//\"/\\\"}
    printf '"%s"' "$value"
}

lock_value() {
    local key=$1
    awk -F '"' -v key="$key" '$1 ~ "^" key "[[:space:]]*=" { print $2; exit }' \
        "$root_dir/sources.lock.toml"
}

if [[ $# -ne 3 ]]; then
    usage >&2
    exit 64
fi

build_dir=$1
bundle_id=$2
stage_dir=$3

[[ $bundle_id =~ ^[a-z0-9]+([.-][a-z0-9]+)*$ ]] || {
    printf 'mesa-adreno: invalid bundle id: %s\n' "$bundle_id" >&2
    exit 64
}
[[ -f $build_dir/build.ninja ]] || {
    printf 'mesa-adreno: not a Meson/Ninja build directory: %s\n' "$build_dir" >&2
    exit 1
}
[[ ! -e $stage_dir ]] || {
    printf 'mesa-adreno: refusing to reuse stage directory: %s\n' "$stage_dir" >&2
    exit 1
}

prefix="/opt/trierarch/mesa/adreno/$bundle_id"
meson_prefix=$(meson introspect --buildoptions "$build_dir" | \
    sed -n 's/.*"name": "prefix", "value": "\([^"]*\)".*/\1/p' | head -n 1)
[[ $meson_prefix == "$prefix" ]] || {
    printf 'mesa-adreno: build prefix is %s, expected %s\n' "$meson_prefix" "$prefix" >&2
    exit 1
}

"$root_dir/scripts/verify-source-lock.sh"
meson install -C "$build_dir" --no-rebuild --destdir "$stage_dir"

bundle_dir="$stage_dir$prefix"
[[ -f $bundle_dir/lib/libEGL_mesa.so.0.0.0 ]] || {
    printf '%s\n' 'mesa-adreno: staged bundle has no libEGL_mesa.so.0.0.0' >&2
    exit 1
}
[[ -f $bundle_dir/lib/dri/libdril_dri.so ]] || {
    printf '%s\n' 'mesa-adreno: staged bundle has no libdril_dri.so' >&2
    exit 1
}

. /etc/os-release
glibc=$(getconf GNU_LIBC_VERSION)
(
    cd "$bundle_dir"
    find lib share -type f -print0 | sort -z | xargs -0 sha256sum > files.sha256
)

cat > "$bundle_dir/manifest.toml" <<EOF
schema = 1

[bundle]
id = $(toml_string "$bundle_id")
target = "arch-aarch64"

[guest]
distro_id = $(toml_string "${ID:-unknown}")
distro_version = $(toml_string "${VERSION_ID:-unknown}")
architecture = $(toml_string "$(uname -m)")
glibc = $(toml_string "$glibc")

[mesa]
remote = $(toml_string "$(lock_value remote)")
revision = $(toml_string "$(lock_value revision)")
release_artifact_sha256 = $(toml_string "$(lock_value release_artifact_sha256)")

[bridge]
abi = 1

[files]
sha256 = "files.sha256"
EOF

printf 'mesa-adreno: staged bundle %s at %s\n' "$bundle_id" "$bundle_dir"
