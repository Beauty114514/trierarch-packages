#!/usr/bin/env bash
set -euo pipefail

export SANITIZE="${SANITIZE:-address}"
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:abort_on_error=1:print_stacktrace=1}"
export LSAN_OPTIONS="${LSAN_OPTIONS:-print_suppressions=1}"

tests="${HOST_TESTS:-${HOST_TEST:-test_server_create test_client_shm test_dmabuf_feedback test_seat_input test_aux_protocols}}"
for test_name in $tests; do
    HOST_TEST="$test_name" "$(dirname "${BASH_SOURCE[0]}")/run.sh"
done
