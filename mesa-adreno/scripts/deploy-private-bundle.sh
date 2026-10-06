#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: deploy-private-bundle.sh [--activate] <stage-dir> <bundle-id>

Copies a verified staging bundle to /opt/trierarch/mesa/adreno/<bundle-id>.
Without --activate it does not change the stable current selector. It never
installs files into /usr or replaces a guest distribution Mesa package.
EOF
}

activate=false
if [[ ${1:-} == --activate ]]; then
    activate=true
    shift
fi
if [[ $# -ne 2 ]]; then
    usage >&2
    exit 64
fi

stage_dir=$1
bundle_id=$2
[[ $bundle_id =~ ^[a-z0-9]+([.-][a-z0-9]+)*$ ]] || {
    printf 'mesa-adreno: invalid bundle id: %s\n' "$bundle_id" >&2
    exit 64
}

source_dir="$stage_dir/opt/trierarch/mesa/adreno/$bundle_id"
root_dir=${TRIERARCH_MESA_ROOT:-/opt/trierarch/mesa/adreno}
destination="$root_dir/$bundle_id"
[[ -f $source_dir/manifest.toml && -f $source_dir/files.sha256 ]] || {
    printf 'mesa-adreno: invalid staged bundle: %s\n' "$source_dir" >&2
    exit 1
}

mkdir -p "$root_dir"
if [[ -e $destination || -L $destination ]]; then
    "$activate" || {
        printf 'mesa-adreno: destination already exists: %s\n' "$destination" >&2
        exit 1
    }
    [[ -f $destination/manifest.toml && -f $destination/files.sha256 ]] || {
        printf 'mesa-adreno: installed bundle is invalid: %s\n' "$destination" >&2
        exit 1
    }
    (
        cd "$destination"
        sha256sum --check files.sha256
    )
else
    temporary="$root_dir/.${bundle_id}.install.$$"
    trap 'rm -rf -- "$temporary"' EXIT
    cp -a -- "$source_dir" "$temporary"
    (
        cd "$temporary"
        sha256sum --check files.sha256
    )
    mv -- "$temporary" "$destination"
    trap - EXIT
fi

if "$activate"; then
    link="$root_dir/.current.new.$$"
    ln -s -- "$bundle_id" "$link"
    mv -Tf -- "$link" "$root_dir/current"
fi

printf 'mesa-adreno: installed private bundle at %s\n' "$destination"
if "$activate"; then
    printf 'mesa-adreno: activated %s\n' "$bundle_id"
else
    printf '%s\n' 'mesa-adreno: not activated; pass --activate only after validation.'
fi
