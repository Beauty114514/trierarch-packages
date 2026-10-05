#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=source-lock.sh
source "$script_dir/source-lock.sh"

mesa_adreno_verify_lock
printf 'mesa-adreno: source lock verified: %s\n' "$(mesa_adreno_lock_value revision)"
