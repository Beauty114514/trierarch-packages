#!/usr/bin/env bash
# Build Mesa 26.2.5 with the Wayland EGL platform for the GL-app experiment.
# KWin keeps using /opt/trierarch/mesa; this installs to a separate prefix.
set -euo pipefail
package_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
src=/tmp/mesa26-src
build=/tmp/mesa26-wayland-build
prefix=/opt/trierarch/mesa-wayland

for tool in git meson ninja pkg-config; do
  command -v "$tool" >/dev/null || { echo "missing $tool" >&2; exit 1; }
done
if ! pkg-config --exists wayland-client wayland-protocols; then
  echo "Missing Wayland development files; installing wayland + wayland-protocols" >&2
  pacman -S --needed --noconfirm wayland wayland-protocols
fi

if [[ ! -d "$src/.git" ]]; then
  rm -rf "$src"
  git clone --depth 1 --branch mesa-26.2.5 \
    https://gitlab.freedesktop.org/mesa/mesa.git "$src"
  patch -d "$src" -p1 < "$package_dir/patches/0001-virgl-vtest-dmabuf-import.patch"
fi

rm -rf "$build"
meson setup "$build" "$src" --prefix="$prefix" --buildtype=release \
  -Dgallium-drivers=freedreno,llvmpipe,virgl -Dfreedreno-kmds=kgsl \
  -Dvulkan-drivers= -Dglx=disabled -Dglvnd=disabled \
  -Degl=enabled -Dgles2=enabled -Dgbm=enabled -Dplatforms=wayland \
  -Dvideo-codecs= -Dgallium-va=disabled -Dhtml-docs=disabled \
  -Dlibunwind=disabled -Db_ndebug=true
ninja -C "$build" -j4
ninja -C "$build" install
echo MESA26_WAYLAND_BUILD_OK
