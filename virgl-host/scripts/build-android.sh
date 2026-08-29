#!/usr/bin/env bash
set -euo pipefail

package_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sources_dir="$package_dir/sources"
virgl_source="$sources_dir/virglrenderer"
epoxy_source="$sources_dir/libepoxy"
angle_archive="$sources_dir/archives/angle-android-arm64.deb"
build_dir="$package_dir/build/android/arm64-v8a"
prefix="$build_dir/prefix"
dist_dir="$package_dir/dist/android/arm64-v8a"
android_api="${ANDROID_API:-24}"
ndk="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"

[[ -f "$sources_dir/.trierarch-patches-applied" ]] || {
    echo "Patched sources are missing. Run scripts/fetch.sh, then scripts/patch.sh." >&2
    exit 1
}
[[ -f "$angle_archive" ]] || {
    echo "ANGLE archive is missing. Run scripts/fetch.sh first." >&2
    exit 1
}
if [[ -z "$ndk" ]]; then
    ndk="$(find "${ANDROID_HOME:-$HOME/Android/Sdk}/ndk" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | sort -V | tail -1 || true)"
fi
[[ -d "$ndk" ]] || {
    echo "Android NDK not found; set ANDROID_NDK_HOME." >&2
    exit 1
}

toolchain="$ndk/toolchains/llvm/prebuilt/linux-x86_64"
target="aarch64-linux-android"
clang="$toolchain/bin/${target}${android_api}-clang"
clangxx="$toolchain/bin/${target}${android_api}-clang++"
llvm_ar="$toolchain/bin/llvm-ar"
llvm_strip="$toolchain/bin/llvm-strip"
sysroot="$toolchain/sysroot"

for tool in "$clang" "$clangxx" "$llvm_ar" "$llvm_strip" ar meson ninja pkg-config python3 tar; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "Missing required tool: $tool" >&2
        exit 1
    }
done

rm -rf "$build_dir" "$dist_dir"
mkdir -p "$build_dir" "$prefix" "$dist_dir"

# virglrenderer generates Gallium protocol sources through Python + PyYAML.
# Keep that build-only dependency isolated from the host Python installation,
# as in the old Trierarch builder.
venv_dir="$build_dir/python-venv"
python3 -m venv "$venv_dir"
"$venv_dir/bin/python" -c 'import yaml' 2>/dev/null || {
    echo 'Installing PyYAML into the VirGL build venv...'
    "$venv_dir/bin/python" -m pip install --upgrade pip
    "$venv_dir/bin/python" -m pip install PyYAML
}
export PATH="$venv_dir/bin:$PATH"

host_pkgconfig="$(command -v pkg-config)"
pkgconfig_wrapper="$build_dir/android-pkg-config"
printf '%s\n' \
    '#!/bin/sh' \
    'export PKG_CONFIG_DIR=' \
    "export PKG_CONFIG_LIBDIR='$prefix/lib/pkgconfig'" \
    "exec '$host_pkgconfig' \"\$@\"" \
    > "$pkgconfig_wrapper"
chmod +x "$pkgconfig_wrapper"

cross_file="$build_dir/cross-android-arm64.txt"
printf '%s\n' \
    '[binaries]' \
    "c = '$clang'" \
    "cpp = '$clangxx'" \
    "ar = '$llvm_ar'" \
    "strip = '$llvm_strip'" \
    "pkg-config = '$pkgconfig_wrapper'" \
    '' \
    '[host_machine]' \
    "system = 'android'" \
    "cpu_family = 'aarch64'" \
    "cpu = 'arm64'" \
    "endian = 'little'" \
    '' \
    '[built-in options]' \
    "c_link_args = ['-llog']" \
    > "$cross_file"

echo '=== Extracting ANGLE runtime libraries ==='
angle_work="$build_dir/angle-work"
angle_dir="$build_dir/angle"
mkdir -p "$angle_work" "$angle_dir"
(
    cd "$angle_work"
    ar x "$angle_archive"
)
angle_data="$(find "$angle_work" -maxdepth 1 -type f -name 'data.tar.*' -print -quit)"
[[ -n "$angle_data" ]] || { echo 'ANGLE package has no data archive.' >&2; exit 1; }
tar --extract --file "$angle_data" --directory "$angle_work"
angle_root="$(find "$angle_work" -type d -name angle-android -print -quit)"
[[ -n "$angle_root" ]] || { echo 'ANGLE package has no angle-android directory.' >&2; exit 1; }
for backend in gl vulkan vulkan-null; do
    [[ -d "$angle_root/$backend" ]] || continue
    mkdir -p "$angle_dir/$backend"
    cp -a "$angle_root/$backend/"*.so "$angle_dir/$backend/"
done
[[ -f "$angle_dir/vulkan/libEGL_angle.so" ]] || {
    echo 'ANGLE Vulkan EGL library was not found.' >&2
    exit 1
}

echo '=== Building libepoxy ==='
epoxy_build="$build_dir/libepoxy"
meson setup "$epoxy_build" "$epoxy_source" \
    --cross-file "$cross_file" --prefix "$prefix" --libdir lib \
    -Degl=yes -Dglx=no -Dx11=false
meson compile -C "$epoxy_build"
meson install -C "$epoxy_build"

pkgconfig_dir="$prefix/lib/pkgconfig"
ndk_libdir="$sysroot/usr/lib/$target/$android_api"
ndk_includedir="$sysroot/usr/include"
mkdir -p "$pkgconfig_dir"
printf '%s\n' \
    "prefix=$sysroot/usr" \
    "libdir=$ndk_libdir" \
    "includedir=$ndk_includedir" \
    '' 'Name: egl' 'Description: Android NDK EGL' 'Version: 1.5' \
    'Libs: -L${libdir} -lEGL' 'Cflags: -I${includedir}' \
    > "$pkgconfig_dir/egl.pc"
printf '%s\n' \
    "prefix=$sysroot/usr" \
    "libdir=$ndk_libdir" \
    "includedir=$ndk_includedir" \
    '' 'Name: glesv2' 'Description: Android NDK GLESv2' 'Version: 3.2' \
    'Libs: -L${libdir} -lGLESv2' 'Cflags: -I${includedir}' \
    > "$pkgconfig_dir/glesv2.pc"

echo '=== Building virglrenderer ==='
virgl_build="$build_dir/virglrenderer"
meson setup "$virgl_build" "$virgl_source" \
    --cross-file "$cross_file" --prefix "$prefix" --libdir lib \
    -Dplatforms=egl -Dcheck-gl-errors=false -Dvenus=true \
    -Drender-server-worker=thread
meson compile -C "$virgl_build"
meson install -C "$virgl_build"

install -Dm755 "$prefix/bin/virgl_test_server" "$dist_dir/virgl_test_server_android"
"$llvm_strip" "$dist_dir/virgl_test_server_android"
install -Dm755 "$prefix/lib/libvirglrenderer.so" "$dist_dir/libvirglrenderer.so"
install -Dm755 "$prefix/lib/libepoxy.so" "$dist_dir/libepoxy.so"
"$llvm_strip" "$dist_dir/libvirglrenderer.so" "$dist_dir/libepoxy.so"

if [[ -f "$prefix/libexec/virgl_render_server" ]]; then
    install -Dm755 "$prefix/libexec/virgl_render_server" "$dist_dir/virgl_render_server"
    "$llvm_strip" "$dist_dir/virgl_render_server"
fi
for backend in gl vulkan vulkan-null; do
    [[ -d "$angle_dir/$backend" ]] || continue
    install -d "$dist_dir/angle/$backend"
    cp -a "$angle_dir/$backend/." "$dist_dir/angle/$backend/"
    "$llvm_strip" "$dist_dir/angle/$backend/"*.so || true
done

echo "Android VirGL artifacts written to $dist_dir"
