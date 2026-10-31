#!/usr/bin/env bash
set -euo pipefail

package_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
sdk="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}"
build_tools="$(find "$sdk/build-tools" -mindepth 1 -maxdepth 1 -type d | sort -V | tail -1)"
platform="$(find "$sdk/platforms" -mindepth 1 -maxdepth 1 -type d | sort -V | tail -1)"
ndk="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
if [[ -z "$ndk" ]]; then
    ndk="$(find "$sdk/ndk" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | sort -V | tail -1 || true)"
fi
[[ -d "$build_tools" && -d "$platform" && -d "$ndk" ]] || {
    echo "Android SDK build-tools, platform, and NDK are required" >&2
    exit 1
}
android_jar="$platform/android.jar"
if ! command -v javac >/dev/null 2>&1; then
    JAVA_HOME="${JAVA_HOME:-/opt/android-studio/jbr}"
    export JAVA_HOME
    export PATH="$JAVA_HOME/bin:$PATH"
fi
work="$package_dir/build"
rm -rf "$work"
mkdir -p "$work/classes" "$work/dex" "$work/apk"

"$ndk/ndk-build" \
    NDK_PROJECT_PATH="$package_dir" \
    APP_BUILD_SCRIPT="$package_dir/jni/Android.mk" \
    NDK_APPLICATION_MK="$package_dir/jni/Application.mk" \
    NDK_LIBS_OUT="$package_dir/libs" \
    NDK_OUT="$work/obj" \
    -j"$(nproc)"

javac -source 8 -target 8 \
    -bootclasspath "$android_jar" \
    -d "$work/classes" \
    $(find "$package_dir/src" -name '*.java')

"$build_tools/d8" --release --min-api 29 --lib "$android_jar" \
    --output "$work/dex" $(find "$work/classes" -name '*.class')

"$build_tools/aapt2" link \
    -o "$work/apk/unsigned.apk" \
    -I "$android_jar" \
    --manifest "$package_dir/AndroidManifest.xml" \
    --min-sdk-version 29 \
    --target-sdk-version 36

cp "$work/dex/classes.dex" "$work/apk/classes.dex"
mkdir -p "$work/apk/lib/arm64-v8a"
cp "$package_dir/libs/arm64-v8a/libtrierarch_scanout_test.so" \
    "$work/apk/lib/arm64-v8a/"
(cd "$work/apk" && zip -q -u unsigned.apk classes.dex)
(cd "$work/apk" && zip -q -u unsigned.apk lib/arm64-v8a/libtrierarch_scanout_test.so)

"$build_tools/zipalign" -f 4 "$work/apk/unsigned.apk" "$work/apk/aligned.apk"
"$build_tools/apksigner" sign \
    --ks "${DEBUG_KEYSTORE:-$HOME/.android/debug.keystore}" \
    --ks-pass pass:android --key-pass pass:android \
    --out "$package_dir/build/trierarch-devicetest.apk" \
    "$work/apk/aligned.apk"

echo "$package_dir/build/trierarch-devicetest.apk"
