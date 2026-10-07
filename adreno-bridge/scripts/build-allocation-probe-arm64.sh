#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
output_path=${TRIERARCH_ADRENO_BRIDGE_PROBE_OUTPUT:-/tmp/trierarch-adreno-bridge-probe}

aarch64-linux-gnu-gcc -O2 -Wall -Wextra -Werror -std=c11 \
    -I"$project_dir" "$project_dir/guest/allocation_probe.c" \
    -o "$output_path"
printf 'Built %s\n' "$output_path"
