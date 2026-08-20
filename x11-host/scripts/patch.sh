#!/usr/bin/env bash
set -euo pipefail

package_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="$package_dir/src/lorie"
series="$package_dir/patches/series"
marker="$source_dir/.trierarch-patches-applied"

[[ -d "$source_dir" ]] || {
    echo "Sources are missing. Run scripts/fetch-lorie.sh first." >&2
    exit 1
}
[[ -f "$series" ]] || {
    echo "Patch series is missing: $series" >&2
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

while read -r patch_name; do
    [[ -z "$patch_name" || "$patch_name" == \#* ]] && continue
    patch_file="$package_dir/patches/$patch_name"
    [[ -f "$patch_file" ]] || {
        echo "Invalid patch series entry: $patch_name" >&2
        exit 1
    }
    patch --batch --forward -d "$source_dir" -p1 < "$patch_file"
done < "$series"

touch "$marker"
echo "Applied Trierarch X11 patches."
