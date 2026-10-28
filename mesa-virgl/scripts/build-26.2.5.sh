#!/usr/bin/env bash
# Build the guest Mesa used by the vtest/virpipe POC.  Run this inside the
# Arch guest; it installs to /opt/trierarch/mesa so the container rootfs keeps
# the result across /tmp resets.
set -euo pipefail
package_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
src=/tmp/mesa26-src
build=/tmp/mesa26-build
prefix=/opt/trierarch/mesa
rm -rf "$src" "$build"
git clone --depth 1 --branch mesa-26.2.5 \
  https://gitlab.freedesktop.org/mesa/mesa.git "$src"
cd "$src"
patch -p1 < "$package_dir/patches/0001-virgl-vtest-dmabuf-import.patch"
meson setup "$build" --prefix="$prefix" --buildtype=release \
  -Dgallium-drivers=freedreno,llvmpipe,virgl -Dfreedreno-kmds=kgsl \
  -Dvulkan-drivers= -Dglx=disabled -Dglvnd=disabled \
  -Degl=enabled -Dgles2=enabled -Dgbm=enabled -Dplatforms= \
  -Dvideo-codecs= -Dgallium-va=disabled -Dhtml-docs=disabled \
  -Dlibunwind=disabled -Db_ndebug=true
ninja -C "$build" -j4
ninja -C "$build" install
echo MESA26_BUILD_OK
