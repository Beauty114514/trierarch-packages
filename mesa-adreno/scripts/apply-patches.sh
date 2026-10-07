#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
patch_dir="$root_dir/patches"

[[ $# -eq 1 ]] || {
    printf '%s\n' 'usage: apply-patches.sh <locked-mesa-source-dir>' >&2
    exit 64
}

source_dir=$1
[[ -e $source_dir/.git ]] || {
    printf 'mesa-adreno: not a Mesa worktree: %s\n' "$source_dir" >&2
    exit 64
}

while IFS= read -r -d '' patch; do
    if git -C "$source_dir" apply --reverse --check "$patch" >/dev/null 2>&1; then
        printf 'mesa-adreno: already applied %s\n' "$(basename -- "$patch")"
        continue
    fi
    git -C "$source_dir" apply --check "$patch"
    git -C "$source_dir" apply "$patch"
    printf 'mesa-adreno: applied %s\n' "$(basename -- "$patch")"
done < <(find "$patch_dir" -maxdepth 1 -type f -name '*.patch' -print0 | sort -z)
