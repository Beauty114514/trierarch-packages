#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=source-lock.sh
source "$script_dir/source-lock.sh"

print_source() {
    mesa_adreno_verify_lock
    printf 'remote=%s\n' "$(mesa_adreno_lock_value remote)"
    printf 'tag=%s\n' "$(mesa_adreno_lock_value tag)"
    printf 'revision=%s\n' "$(mesa_adreno_lock_value revision)"
}

prepare_source() {
    local root_dir cache_dir work_dir remote tag revision actual
    root_dir=$(mesa_adreno_root)
    cache_dir=${MESA_ADRENO_CACHE_DIR:-"$root_dir/cache/mesa-for-android-container"}
    work_dir=${MESA_ADRENO_WORK_DIR:-"$root_dir/work/mesa-$(mesa_adreno_lock_value revision)"}
    remote=$(mesa_adreno_lock_value remote)
    tag=$(mesa_adreno_lock_value tag)
    revision=$(mesa_adreno_lock_value revision)

    mesa_adreno_verify_lock
    mkdir -p "$(dirname -- "$cache_dir")" "$(dirname -- "$work_dir")"

    if [[ ! -d $cache_dir/.git ]]; then
        git clone --no-checkout "$remote" "$cache_dir"
    fi

    git -C "$cache_dir" fetch --no-tags origin "refs/tags/$tag:refs/tags/$tag"
    actual=$(git -C "$cache_dir" rev-parse "$tag^{commit}")
    if [[ $actual != "$revision" ]]; then
        printf 'mesa-adreno: tag %s resolved to %s, expected %s\n' "$tag" "$actual" "$revision" >&2
        exit 1
    fi

    if [[ -e $work_dir ]]; then
        actual=$(git -C "$work_dir" rev-parse HEAD)
        if [[ $actual != "$revision" ]]; then
            printf 'mesa-adreno: refusing to reuse worktree at %s (HEAD=%s, expected=%s)\n' "$work_dir" "$actual" "$revision" >&2
            exit 1
        fi
    else
        git -C "$cache_dir" worktree add --detach "$work_dir" "$revision"
    fi

    printf '%s\n' "$work_dir"
}

case ${1:---help} in
    --print-source)
        print_source
        ;;
    --prepare)
        prepare_source
        ;;
    --help)
        cat <<'EOF'
Usage: prepare-source.sh --print-source | --prepare

--print-source  Show the locked upstream identity without changing files.
--prepare       Clone/fetch the locked tag into mesa-adreno/cache and create a
                detached, patchable worktree under mesa-adreno/work.
EOF
        ;;
    *)
        printf 'unknown argument: %s\n' "$1" >&2
        exit 64
        ;;
esac
