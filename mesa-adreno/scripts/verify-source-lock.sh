#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
lock_file="$root_dir/sources.lock.toml"

value() {
    local key=$1
    sed -n "s/^${key} = \"\(.*\)\"$/\1/p" "$lock_file" | head -n 1
}

state=$(value state)
revision=$(value revision)
release_artifact_sha256=$(value release_artifact_sha256)

if [[ $state != "resolved" || ! $revision =~ ^[[:xdigit:]]{40}$ || ! $release_artifact_sha256 =~ ^[[:xdigit:]]{64}$ ]]; then
    cat >&2 <<EOF
mesa-adreno: source lock is invalid.
Record the exact mesa-for-android-container revision and published release
archive SHA-256 in $lock_file before building a bundle.
EOF
    exit 2
fi

printf 'mesa-adreno: source lock verified: %s\n' "$revision"
