#!/usr/bin/env bash
set -euo pipefail

package_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
destination="$package_dir/src/lorie"
upstream_url="https://github.com/termux/termux-x11.git"
upstream_commit="9471ad977d21b7c7bec45117008f74f330d45983"

if [[ -e "$destination" ]]; then
    echo "Refusing to overwrite existing source: $destination" >&2
    echo "Remove it first if it is a disposable upstream snapshot." >&2
    exit 1
fi

work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT

git clone --filter=blob:none --no-checkout "$upstream_url" "$work_dir/termux-x11"
git -C "$work_dir/termux-x11" checkout --detach "$upstream_commit"
git -C "$work_dir/termux-x11" submodule update --init --recursive

mkdir -p "$destination"
cp -a "$work_dir/termux-x11/lorie/src/main/cpp/." "$destination/"
find "$destination" -name .git -type f -delete
find "$destination" -type d -name .git -prune -exec rm -rf {} +

echo "Prepared Lorie source at $destination"
