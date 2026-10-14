#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
package_dir=${1:-"$script_dir/../.debian-build"}

shopt -s nullglob
packages=("$package_dir"/*.deb)
if ((${#packages[@]} == 0)); then
    printf '%s\n' 'No Debian Mesa packages found beside the source directory.' >&2
    exit 1
fi

exec sudo dpkg -i "${packages[@]}"
