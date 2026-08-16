#!/usr/bin/env bash
set -euo pipefail

package_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
gradle_home="${GRADLE_USER_HOME:-$package_dir/build/gradle-home}"
output_dir="$package_dir/dist/android/arm64-v8a"

cd "$package_dir"

export ANDROID_HOME="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
export ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-$ANDROID_HOME}"
export GRADLE_USER_HOME="$gradle_home"

if [[ -z "$ANDROID_HOME" || ! -d "$ANDROID_HOME/ndk/29.0.14206865" ]]; then
    echo "Android NDK 29.0.14206865 is required." >&2
    exit 1
fi

"$package_dir/gradlew" --no-daemon externalNativeBuildRelease

source_so="$(find "$package_dir/.cxx" "$package_dir/build" -type f -name libXlorie.so -print -quit)"
if [[ -z "$source_so" ]]; then
    echo "libXlorie.so was not produced." >&2
    exit 1
fi

install -Dm755 "$source_so" "$output_dir/libXlorie.so"
