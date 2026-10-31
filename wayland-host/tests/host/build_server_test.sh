#!/usr/bin/env bash
set -euo pipefail

package_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="$package_dir/build/host-tests"
cd "$package_dir"
protocol_dir="$build_dir/protocol-generated"
mkdir -p "$build_dir"
"$package_dir/tests/host/generate-protocols.sh" "$protocol_dir"

cc="${CC:-cc}"
server_cflags="$(pkg-config --cflags wayland-server)"
server_libs="$(pkg-config --libs wayland-server)"
client_cflags="$(pkg-config --cflags wayland-client)"
client_libs="$(pkg-config --libs wayland-client)"
sources=(
    src/buffer_shm.c
    src/compositor.c
    src/dmabuf_feedback_device.c
    src/dmabuf_feedback_path.c
    src/egl_buffer.c
    src/explicit_sync.c
    src/keycode_map.c
    src/layer_shell.c
    src/linux_dmabuf.c
    src/output.c
    src/pointer_constraints.c
    src/presentation.c
    src/protocol_stubs.c
    src/seat.c
    src/single_pixel_buffer.c
    src/subcompositor.c
    src/surface.c
    src/viewporter.c
    src/xdg_decoration.c
    src/xdg_output.c
    src/xdg_shell.c
)
protocol_sources=("$protocol_dir"/*-protocol.c)
stubs=(
    tests/host/stubs/ahb_stub.c
    tests/host/stubs/tvg_stub.c
    tests/host/stubs/renderer_stub.c
)
sources_paths=()
for source in "${sources[@]}"; do
    sources_paths+=("$package_dir/$source")
done
"$cc" -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
    -D_GNU_SOURCE \
    -I"$package_dir/src" \
    -I"$protocol_dir" \
    -I"$package_dir/../dmabuf-device-report/include" \
    -I"$package_dir/tests/host/stubs" \
    $server_cflags \
    "${sources_paths[@]}" \
    "${protocol_sources[@]}" \
    "${stubs[@]}" \
    "$package_dir/tests/host/test_server_create.c" \
    -ldl $server_libs -o "$build_dir/test_server_create"

"$build_dir/test_server_create"

"$cc" -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
    -D_GNU_SOURCE -pthread \
    -I"$package_dir/src" \
    -I"$protocol_dir" \
    -I"$package_dir/../dmabuf-device-report/include" \
    -I"$package_dir/tests/host/stubs" \
    $client_cflags $server_cflags \
    "${sources_paths[@]}" \
    "${protocol_sources[@]}" \
    "${stubs[@]}" \
    "$package_dir/tests/host/test_client_shm.c" \
    -ldl $client_libs $server_libs -o "$build_dir/test_client_shm"

"$cc" -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
    -D_GNU_SOURCE -pthread \
    -I"$package_dir/src" \
    -I"$protocol_dir" \
    -I"$package_dir/../dmabuf-device-report/include" \
    -I"$package_dir/tests/host/stubs" \
    $client_cflags $server_cflags \
    "${sources_paths[@]}" \
    "${protocol_sources[@]}" \
    "${stubs[@]}" \
    "$package_dir/tests/host/test_dmabuf_feedback.c" \
    -ldl $client_libs $server_libs -o "$build_dir/test_dmabuf_feedback"

"$cc" -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
    -D_GNU_SOURCE -pthread \
    -I"$package_dir/src" \
    -I"$protocol_dir" \
    -I"$package_dir/../dmabuf-device-report/include" \
    -I"$package_dir/tests/host/stubs" \
    $client_cflags $server_cflags \
    "${sources_paths[@]}" \
    "${protocol_sources[@]}" \
    "${stubs[@]}" \
    "$package_dir/tests/host/test_seat_input.c" \
    -ldl $client_libs $server_libs -o "$build_dir/test_seat_input"

"$cc" -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
    -D_GNU_SOURCE -pthread \
    -I"$package_dir/src" \
    -I"$protocol_dir" \
    -I"$package_dir/../dmabuf-device-report/include" \
    -I"$package_dir/tests/host/stubs" \
    $client_cflags $server_cflags \
    "${sources_paths[@]}" \
    "${protocol_sources[@]}" \
    "${stubs[@]}" \
    "$package_dir/tests/host/test_aux_protocols.c" \
    -ldl $client_libs $server_libs -o "$build_dir/test_aux_protocols"

"$build_dir/test_aux_protocols"

"$build_dir/test_seat_input"

"$cc" -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
    -D_GNU_SOURCE -pthread \
    -I"$package_dir/src" \
    -I"$protocol_dir" \
    -I"$package_dir/../dmabuf-device-report/include" \
    -I"$package_dir/tests/host/stubs" \
    $client_cflags $server_cflags \
    "${sources_paths[@]}" \
    "${protocol_sources[@]}" \
    "${stubs[@]}" \
    "$package_dir/tests/host/test_aux_protocols.c" \
    -ldl $client_libs $server_libs -o "$build_dir/test_aux_protocols"

"$build_dir/test_aux_protocols"

"$build_dir/test_dmabuf_feedback"

"$cc" -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
    -D_GNU_SOURCE -pthread \
    -I"$package_dir/src" \
    -I"$protocol_dir" \
    -I"$package_dir/../dmabuf-device-report/include" \
    -I"$package_dir/tests/host/stubs" \
    $client_cflags $server_cflags \
    "${sources_paths[@]}" \
    "${protocol_sources[@]}" \
    "${stubs[@]}" \
    "$package_dir/tests/host/test_seat_input.c" \
    -ldl $client_libs $server_libs -o "$build_dir/test_seat_input"

"$cc" -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
    -D_GNU_SOURCE -pthread \
    -I"$package_dir/src" \
    -I"$protocol_dir" \
    -I"$package_dir/../dmabuf-device-report/include" \
    -I"$package_dir/tests/host/stubs" \
    $client_cflags $server_cflags \
    "${sources_paths[@]}" \
    "${protocol_sources[@]}" \
    "${stubs[@]}" \
    "$package_dir/tests/host/test_aux_protocols.c" \
    -ldl $client_libs $server_libs -o "$build_dir/test_aux_protocols"

"$build_dir/test_aux_protocols"

"$build_dir/test_seat_input"

"$cc" -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
    -D_GNU_SOURCE -pthread \
    -I"$package_dir/src" \
    -I"$protocol_dir" \
    -I"$package_dir/../dmabuf-device-report/include" \
    -I"$package_dir/tests/host/stubs" \
    $client_cflags $server_cflags \
    "${sources_paths[@]}" \
    "${protocol_sources[@]}" \
    "${stubs[@]}" \
    "$package_dir/tests/host/test_aux_protocols.c" \
    -ldl $client_libs $server_libs -o "$build_dir/test_aux_protocols"

"$build_dir/test_aux_protocols"

"$build_dir/test_client_shm"

"$cc" -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
    -D_GNU_SOURCE -pthread \
    -I"$package_dir/src" \
    -I"$protocol_dir" \
    -I"$package_dir/../dmabuf-device-report/include" \
    -I"$package_dir/tests/host/stubs" \
    $client_cflags $server_cflags \
    "${sources_paths[@]}" \
    "${protocol_sources[@]}" \
    "${stubs[@]}" \
    "$package_dir/tests/host/test_dmabuf_feedback.c" \
    -ldl $client_libs $server_libs -o "$build_dir/test_dmabuf_feedback"

"$cc" -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
    -D_GNU_SOURCE -pthread \
    -I"$package_dir/src" \
    -I"$protocol_dir" \
    -I"$package_dir/../dmabuf-device-report/include" \
    -I"$package_dir/tests/host/stubs" \
    $client_cflags $server_cflags \
    "${sources_paths[@]}" \
    "${protocol_sources[@]}" \
    "${stubs[@]}" \
    "$package_dir/tests/host/test_seat_input.c" \
    -ldl $client_libs $server_libs -o "$build_dir/test_seat_input"

"$cc" -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
    -D_GNU_SOURCE -pthread \
    -I"$package_dir/src" \
    -I"$protocol_dir" \
    -I"$package_dir/../dmabuf-device-report/include" \
    -I"$package_dir/tests/host/stubs" \
    $client_cflags $server_cflags \
    "${sources_paths[@]}" \
    "${protocol_sources[@]}" \
    "${stubs[@]}" \
    "$package_dir/tests/host/test_aux_protocols.c" \
    -ldl $client_libs $server_libs -o "$build_dir/test_aux_protocols"

"$build_dir/test_aux_protocols"

"$build_dir/test_seat_input"

"$cc" -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
    -D_GNU_SOURCE -pthread \
    -I"$package_dir/src" \
    -I"$protocol_dir" \
    -I"$package_dir/../dmabuf-device-report/include" \
    -I"$package_dir/tests/host/stubs" \
    $client_cflags $server_cflags \
    "${sources_paths[@]}" \
    "${protocol_sources[@]}" \
    "${stubs[@]}" \
    "$package_dir/tests/host/test_aux_protocols.c" \
    -ldl $client_libs $server_libs -o "$build_dir/test_aux_protocols"

"$build_dir/test_aux_protocols"

"$build_dir/test_dmabuf_feedback"

"$cc" -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
    -D_GNU_SOURCE -pthread \
    -I"$package_dir/src" \
    -I"$protocol_dir" \
    -I"$package_dir/../dmabuf-device-report/include" \
    -I"$package_dir/tests/host/stubs" \
    $client_cflags $server_cflags \
    "${sources_paths[@]}" \
    "${protocol_sources[@]}" \
    "${stubs[@]}" \
    "$package_dir/tests/host/test_seat_input.c" \
    -ldl $client_libs $server_libs -o "$build_dir/test_seat_input"

"$cc" -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
    -D_GNU_SOURCE -pthread \
    -I"$package_dir/src" \
    -I"$protocol_dir" \
    -I"$package_dir/../dmabuf-device-report/include" \
    -I"$package_dir/tests/host/stubs" \
    $client_cflags $server_cflags \
    "${sources_paths[@]}" \
    "${protocol_sources[@]}" \
    "${stubs[@]}" \
    "$package_dir/tests/host/test_aux_protocols.c" \
    -ldl $client_libs $server_libs -o "$build_dir/test_aux_protocols"

"$build_dir/test_aux_protocols"

"$build_dir/test_seat_input"

"$cc" -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
    -D_GNU_SOURCE -pthread \
    -I"$package_dir/src" \
    -I"$protocol_dir" \
    -I"$package_dir/../dmabuf-device-report/include" \
    -I"$package_dir/tests/host/stubs" \
    $client_cflags $server_cflags \
    "${sources_paths[@]}" \
    "${protocol_sources[@]}" \
    "${stubs[@]}" \
    "$package_dir/tests/host/test_aux_protocols.c" \
    -ldl $client_libs $server_libs -o "$build_dir/test_aux_protocols"

"$build_dir/test_aux_protocols"
