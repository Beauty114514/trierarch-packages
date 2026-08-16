#!/usr/bin/env bash
set -euo pipefail

package_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sources_dir="$package_dir/sources"
series="$package_dir/patches/series"
marker="$sources_dir/.trierarch-patches-applied"

[[ -d "$sources_dir/proot" && -d "$sources_dir/libandroid-shmem" ]] || {
    echo "Sources are missing. Run scripts/fetch.sh first." >&2
    exit 1
}
[[ ! -e "$marker" ]] || {
    echo "Patches are already applied. Fetch clean sources before applying again." >&2
    exit 1
}
command -v patch >/dev/null 2>&1 || {
    echo "Missing required tool: patch" >&2
    exit 1
}

while read -r source_name patch_name; do
    [[ -z "$source_name" || "$source_name" == \#* ]] && continue
    source_dir="$sources_dir/$source_name"
    patch_file="$package_dir/patches/$patch_name"
    [[ -d "$source_dir" && -f "$patch_file" ]] || {
        echo "Invalid patch series entry: $source_name $patch_name" >&2
        exit 1
    }
    patch --batch --forward -d "$source_dir" -p1 < "$patch_file"
done < "$series"

touch "$marker"
echo "Applied Trierarch patches."
