#!/usr/bin/env sh
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
host_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/trierarch-dmabuf-frame-queue.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

cc -std=c11 -Wall -Wextra -Werror -pthread \
  -I"$host_dir/src" \
  "$host_dir/src/dmabuf_frame_queue.c" \
  "$test_dir/dmabuf_frame_queue_test.c" \
  -o "$build_dir/dmabuf_frame_queue_test"
"$build_dir/dmabuf_frame_queue_test"
