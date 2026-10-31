#!/usr/bin/env bash
set -euo pipefail

export HOST_TEST="${HOST_TEST:-test_server_create}"
export SANITIZE="${SANITIZE:-address}"
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:abort_on_error=1:print_stacktrace=1}"
export LSAN_OPTIONS="${LSAN_OPTIONS:-print_suppressions=1}"

exec "$(dirname "${BASH_SOURCE[0]}")/run.sh"
