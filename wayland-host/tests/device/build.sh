#!/usr/bin/env bash
set -euo pipefail

package_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ndk="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
if [[ -z "$ndk" ]]; then
    ndk="$(find "${ANDROID_HOME:-$HOME/Android/Sdk}/ndk" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | sort -V | tail -1 || true)"
fi
[[ -d "$ndk" ]] || { echo "Android NDK not found; set ANDROID_NDK_HOME" >&2; exit 1; }

out_dir="$package_dir/dist/device"
rm -rf "$out_dir"
mkdir -p "$out_dir"
"$ndk/ndk-build" \
    NDK_PROJECT_PATH="$package_dir/tests/device" \
    APP_BUILD_SCRIPT="$package_dir/tests/device/Android.mk" \
    NDK_APPLICATION_MK="$package_dir/tests/device/Application.mk" \
    NDK_LIBS_OUT="$out_dir" \
    NDK_OUT="$package_dir/build/device-obj" \
    -j"$(nproc)"

echo "device test written to $out_dir/arm64-v8a/trierarch-ahb-egl-test"
