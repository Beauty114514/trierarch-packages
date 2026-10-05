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
source_sha256=$(value source_sha256)

if [[ $state != "resolved" || ! $revision =~ ^[[:xdigit:]]{40}$ || ! $source_sha256 =~ ^[[:xdigit:]]{64}$ ]]; then
    cat >&2 <<EOF
mesa-adreno: source lock is intentionally unresolved.
Record the exact mesa-for-android-container revision and source SHA-256 for
the tested runtime artifact in $lock_file before building a bundle.
EOF
    exit 2
fi

printf 'mesa-adreno: source lock verified: %s\n' "$revision"
