#!/usr/bin/env bash
set -euo pipefail

export SANITIZE="${SANITIZE:-address,undefined}"
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:abort_on_error=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}"

exec "$(dirname "${BASH_SOURCE[0]}")/run.sh"
