#!/usr/bin/env bash
# Shared helpers for scripts that consume mesa-adreno/sources.lock.toml.

mesa_adreno_root() {
    CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd
}

mesa_adreno_lock_value() {
    local key=$1
    local lock_file
    lock_file="$(mesa_adreno_root)/sources.lock.toml"
    sed -n "s/^${key} = \"\(.*\)\"$/\1/p" "$lock_file" | head -n 1
}

mesa_adreno_verify_lock() {
    local state revision release_artifact_sha256
    state=$(mesa_adreno_lock_value state)
    revision=$(mesa_adreno_lock_value revision)
    release_artifact_sha256=$(mesa_adreno_lock_value release_artifact_sha256)

    if [[ $state != "resolved" || ! $revision =~ ^[[:xdigit:]]{40}$ || ! $release_artifact_sha256 =~ ^[[:xdigit:]]{64}$ ]]; then
        cat >&2 <<EOF
mesa-adreno: source lock is invalid.
Record the exact mesa-for-android-container revision and published release
archive SHA-256 in $(mesa_adreno_root)/sources.lock.toml before building a bundle.
EOF
        return 2
    fi
}
