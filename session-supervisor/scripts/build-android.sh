#!/usr/bin/env bash
set -euo pipefail
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd "$script_dir/.." && pwd)"
ndk="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
if [[ -z "$ndk" ]]; then
    ndk="$(find "${ANDROID_HOME:-$HOME/Android/Sdk}/ndk" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | sort -V | tail -1)"
fi
[[ -d "$ndk" ]] || { echo "Android NDK not found" >&2; exit 1; }
clang="$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang"
[[ -x "$clang" ]] || { echo "NDK clang not found: $clang" >&2; exit 1; }
mkdir -p "$project_dir/dist"
"$clang" -std=c11 -O2 -Wall -Wextra -Werror \
    "$project_dir/src/main.c" \
    -o "$project_dir/dist/trierarch-droidspaces-supervisor"
