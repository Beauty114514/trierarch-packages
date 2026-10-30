#!/usr/bin/env bash
set -euo pipefail

package_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="$package_dir/build/host-tests"
mkdir -p "$build_dir"

cc="${CC:-cc}"
"$cc" -std=c11 -Wall -Wextra -Werror \
    -I"$package_dir/src" \
    "$package_dir/tests/host/test_dmabuf_feedback_path.c" \
    "$package_dir/src/dmabuf_feedback_path.c" \
    -o "$build_dir/test_dmabuf_feedback_path"
"$build_dir/test_dmabuf_feedback_path"

"$package_dir/tests/host/build_server_test.sh"
