#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
build_dir="$project_dir/build"
dist_dir="$project_dir/dist"

mkdir -p "$build_dir" "$dist_dir"
cc=${CC:-cc}
"$cc" -std=c11 -Wall -Wextra -Werror \
  "$project_dir/src/main.c" \
  -o "$dist_dir/trierarch-session-supervisor"
