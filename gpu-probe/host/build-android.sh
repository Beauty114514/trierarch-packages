#!/usr/bin/env bash
set -euo pipefail

package_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ndk="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
if [[ -z "$ndk" ]]; then
    ndk="$(find "${ANDROID_HOME:-$HOME/Android/Sdk}/ndk" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | sort -V | tail -1 || true)"
fi
[[ -d "$ndk" ]] || { echo "Android NDK not found; set ANDROID_NDK_HOME" >&2; exit 1; }
clang="$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang"
[[ -x "$clang" ]] || { echo "NDK clang not found" >&2; exit 1; }

out_dir="$package_dir/dist/android/arm64-v8a"
mkdir -p "$out_dir"
"$clang" -shared -fPIC -std=c11 -O2 -Wall -Wextra \
    -I"$package_dir" "$package_dir/host/gpu_probe_host.c" \
    -o "$out_dir/libtrierarch-gpu-probe.so" -llog -lEGL -lGLESv2 -landroid
echo "Android probe written to $out_dir/libtrierarch-gpu-probe.so"
