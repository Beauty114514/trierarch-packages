#!/usr/bin/env bash
set -euo pipefail

source_dir=${1:?usage: apply.sh <mesa-source-dir>}
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

for patch in "$script_dir"/../patches/*.patch; do
    printf 'applying %s\n' "$(basename "$patch")"
    patch -d "$source_dir" -p1 < "$patch"
done
