#!/usr/bin/env bash
set -euo pipefail

package_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="$package_dir/sources"
wayland_src="$source_dir/wayland"
libffi_src="$source_dir/libffi"
protocol_xml="$source_dir/wayland-protocols/stable/xdg-shell/xdg-shell.xml"
single_pixel_xml="$source_dir/wayland-protocols/staging/single-pixel-buffer/single-pixel-buffer-v1.xml"
viewporter_xml="$source_dir/wayland-protocols/stable/viewporter/viewporter.xml"
pointer_constraints_xml="$source_dir/wayland-protocols/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml"
presentation_xml="$source_dir/wayland-protocols/stable/presentation-time/presentation-time.xml"
linux_dmabuf_xml="$source_dir/wayland-protocols/stable/linux-dmabuf/linux-dmabuf-v1.xml"
xdg_output_xml="$source_dir/wayland-protocols/unstable/xdg-output/xdg-output-unstable-v1.xml"
xdg_decoration_xml="$source_dir/wayland-protocols/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml"
fractional_scale_xml="$source_dir/wayland-protocols/staging/fractional-scale/fractional-scale-v1.xml"
relative_pointer_xml="$source_dir/wayland-protocols/unstable/relative-pointer/relative-pointer-unstable-v1.xml"
android_wlegl_xml="$package_dir/protocol/android-wlegl.xml"
build_dir="$package_dir/build"
ffi_prefix="$build_dir/libffi-install"
wayland_build="$build_dir/wayland-android"
wayland_prefix="$build_dir/wayland-install"
protocol_dir="$package_dir/protocol/generated"
out_dir="$package_dir/dist/android/arm64-v8a"

ndk="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
if [[ -z "$ndk" ]]; then
    ndk="$(find "${ANDROID_HOME:-$HOME/Android/Sdk}/ndk" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | sort -V | tail -1 || true)"
fi
[[ -d "$ndk" ]] || { echo "Android NDK not found; set ANDROID_NDK_HOME" >&2; exit 1; }
toolchain="$ndk/toolchains/llvm/prebuilt/linux-x86_64"
clang="$toolchain/bin/aarch64-linux-android26-clang"
[[ -x "$clang" ]] || { echo "NDK toolchain not found: $clang" >&2; exit 1; }

for command_name in meson ninja make wayland-scanner; do
    command -v "$command_name" >/dev/null || { echo "Missing required tool: $command_name" >&2; exit 1; }
done
pkgconfig="$(command -v pkg-config)"
[[ -n "$pkgconfig" ]] || { echo "Missing required tool: pkg-config" >&2; exit 1; }
[[ -d "$wayland_src" && -d "$libffi_src" ]] || { echo "Run scripts/fetch.sh first" >&2; exit 1; }

rm -rf "$ffi_prefix" "$libffi_src/build-android"
mkdir -p "$ffi_prefix" "$libffi_src/build-android"
(
    cd "$libffi_src/build-android"
    CC="$clang" CFLAGS="-DANDROID -fPIC -std=gnu11" LDFLAGS="-fPIC" \
        "$libffi_src/configure" --host=aarch64-linux-android \
        --prefix="$ffi_prefix" --disable-docs --disable-static \
        --disable-multi-os-directory
    make -j"$(nproc)"
    make install
)

cross_file="$build_dir/cross-android-arm64.txt"
mkdir -p "$build_dir"
printf '%s\n' '[binaries]' "c = '$clang'" "cpp = '$toolchain/bin/aarch64-linux-android26-clang++'" "ar = '$toolchain/bin/llvm-ar'" "strip = '$toolchain/bin/llvm-strip'" "pkg-config = '$pkgconfig'" '' '[host_machine]' "system = 'android'" "cpu_family = 'aarch64'" "cpu = 'arm64'" "endian = 'little'" > "$cross_file"

rm -rf "$wayland_build" "$wayland_prefix"
PKG_CONFIG_PATH="$ffi_prefix/lib/pkgconfig" meson setup "$wayland_build" "$wayland_src" \
    --cross-file "$cross_file" --prefix="$wayland_prefix" --libdir=lib \
    -Dlibraries=true -Dscanner=false -Dtests=false -Ddocumentation=false \
    -Ddtd_validation=false
meson compile -C "$wayland_build"
meson install -C "$wayland_build"
cp -a "$ffi_prefix/lib/libffi.so"* "$wayland_prefix/lib/"

rm -rf "$protocol_dir"
mkdir -p "$protocol_dir"
wayland-scanner server-header "$protocol_xml" "$protocol_dir/xdg-shell-server-protocol.h"
wayland-scanner private-code "$protocol_xml" "$protocol_dir/xdg-shell-protocol.c"
wayland-scanner server-header "$single_pixel_xml" "$protocol_dir/single-pixel-buffer-v1-server-protocol.h"
wayland-scanner private-code "$single_pixel_xml" "$protocol_dir/single-pixel-buffer-v1-protocol.c"
wayland-scanner server-header "$viewporter_xml" "$protocol_dir/viewporter-server-protocol.h"
wayland-scanner private-code "$viewporter_xml" "$protocol_dir/viewporter-protocol.c"
wayland-scanner server-header "$pointer_constraints_xml" "$protocol_dir/pointer-constraints-server-protocol.h"
wayland-scanner private-code "$pointer_constraints_xml" "$protocol_dir/pointer-constraints-protocol.c"
wayland-scanner server-header "$presentation_xml" "$protocol_dir/presentation-time-server-protocol.h"
wayland-scanner private-code "$presentation_xml" "$protocol_dir/presentation-time-protocol.c"
wayland-scanner server-header "$linux_dmabuf_xml" "$protocol_dir/linux-dmabuf-v1-server-protocol.h"
wayland-scanner private-code "$linux_dmabuf_xml" "$protocol_dir/linux-dmabuf-v1-protocol.c"
wayland-scanner server-header "$xdg_output_xml" "$protocol_dir/xdg-output-unstable-v1-server-protocol.h"
wayland-scanner private-code "$xdg_output_xml" "$protocol_dir/xdg-output-unstable-v1-protocol.c"
wayland-scanner server-header "$xdg_decoration_xml" "$protocol_dir/xdg-decoration-unstable-v1-server-protocol.h"
wayland-scanner private-code "$xdg_decoration_xml" "$protocol_dir/xdg-decoration-unstable-v1-protocol.c"
wayland-scanner server-header "$fractional_scale_xml" "$protocol_dir/fractional-scale-v1-server-protocol.h"
wayland-scanner private-code "$fractional_scale_xml" "$protocol_dir/fractional-scale-v1-protocol.c"
wayland-scanner server-header "$relative_pointer_xml" "$protocol_dir/relative-pointer-unstable-v1-server-protocol.h"
wayland-scanner private-code "$relative_pointer_xml" "$protocol_dir/relative-pointer-unstable-v1-protocol.c"
wayland-scanner server-header "$android_wlegl_xml" "$protocol_dir/android-wlegl-server-protocol.h"
wayland-scanner private-code "$android_wlegl_xml" "$protocol_dir/android-wlegl-protocol.c"

(cd "$package_dir" && "$ndk/ndk-build" NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=./Android.mk \
    NDK_APPLICATION_MK=./Application.mk NDK_LIBS_OUT="$build_dir/ndk-libs" \
    NDK_OUT="$build_dir/ndk-out" -j"$(nproc)")

rm -rf "$out_dir"
mkdir -p "$out_dir"
cp "$build_dir/ndk-libs/arm64-v8a/libtrierarch-wayland-host.so" "$out_dir/"
cp "$wayland_prefix/lib/libwayland-server.so" "$out_dir/"
cp "$wayland_prefix/lib/libffi.so"* "$out_dir/"
echo "Android artifacts written to $out_dir"
