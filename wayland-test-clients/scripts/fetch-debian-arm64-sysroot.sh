#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
sysroot_dir="${TRIERARCH_WAYLAND_TEST_SYSROOT:-$project_dir/../wayland-ime-bridge/.sysroot/debian-bookworm-arm64}"
downloads="$sysroot_dir/.downloads"

command -v curl >/dev/null || { printf '%s\n' 'curl is required.' >&2; exit 127; }
command -v dpkg-deb >/dev/null || { printf '%s\n' 'dpkg-deb is required.' >&2; exit 127; }

# Keep Wayland client headers and libffi in the shared sysroot.  The lifecycle
# probe and the IME bridge then compile against exactly the same ABI baseline.
TRIERARCH_WAYLAND_IME_SYSROOT="$sysroot_dir" \
    bash "$project_dir/../wayland-ime-bridge/scripts/fetch-debian-arm64-sysroot.sh"

mkdir -p "$downloads"
fetch() {
    archive="$downloads/$1"
    url="$2"
    if [ ! -f "$archive" ]; then
        curl --fail --location --retry 3 --output "$archive" "$url"
    fi
    dpkg-deb --extract "$archive" "$sysroot_dir"
}

mesa_base=https://deb.debian.org/debian/pool/main/m/mesa
drm_base=https://deb.debian.org/debian/pool/main/libd/libdrm
wayland_base=https://deb.debian.org/debian/pool/main/w/wayland
expat_base=https://deb.debian.org/debian/pool/main/e/expat
fetch libgbm-dev_22.3.6-1+deb12u2_arm64.deb \
    "$mesa_base/libgbm-dev_22.3.6-1+deb12u2_arm64.deb"
fetch libgbm1_22.3.6-1+deb12u2_arm64.deb \
    "$mesa_base/libgbm1_22.3.6-1+deb12u2_arm64.deb"
fetch libdrm-dev_2.4.114-1+b1_arm64.deb \
    "$drm_base/libdrm-dev_2.4.114-1+b1_arm64.deb"
fetch libdrm2_2.4.114-1+b1_arm64.deb \
    "$drm_base/libdrm2_2.4.114-1+b1_arm64.deb"
fetch libwayland-server0_1.21.0-1_arm64.deb \
    "$wayland_base/libwayland-server0_1.21.0-1_arm64.deb"
fetch libexpat1_2.5.0-1+deb12u2_arm64.deb \
    "$expat_base/libexpat1_2.5.0-1+deb12u2_arm64.deb"
fetch libexpat1-dev_2.5.0-1+deb12u2_arm64.deb \
    "$expat_base/libexpat1-dev_2.5.0-1+deb12u2_arm64.deb"

test -f "$sysroot_dir/usr/include/gbm.h"
test -f "$sysroot_dir/usr/lib/aarch64-linux-gnu/libgbm.so"
test -f "$sysroot_dir/usr/lib/aarch64-linux-gnu/libdrm.so"
test -f "$sysroot_dir/usr/lib/aarch64-linux-gnu/libwayland-server.so.0"
test -f "$sysroot_dir/usr/lib/aarch64-linux-gnu/libexpat.so"
printf '%s\n' "Prepared Debian Bookworm arm64 GBM test sysroot at $sysroot_dir"
