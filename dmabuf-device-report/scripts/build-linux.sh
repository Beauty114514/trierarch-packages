#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
dist_dir="$project_dir/dist"

mkdir -p "$dist_dir"
cc=${CC:-cc}
"$cc" -std=c11 -Wall -Wextra -Werror \
  -I"$project_dir/include" \
  "$project_dir/src/main.c" \
  -o "$dist_dir/trierarch-dmabuf-device-report"
