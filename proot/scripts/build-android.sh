#!/usr/bin/env bash
set -euo pipefail

package_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sources_dir="$package_dir/sources"
proot_source="$sources_dir/proot"
talloc_source="$sources_dir/talloc"
shmem_source="$sources_dir/libandroid-shmem"
build_dir="$package_dir/build/android/arm64-v8a"
dist_dir="$package_dir/dist/android/arm64-v8a"
android_api="${ANDROID_API:-24}"
ndk="${ANDROID_NDK_ROOT:-${ANDROID_NDK_HOME:-}}"

[[ -f "$sources_dir/.trierarch-patches-applied" ]] || {
    echo "Patched sources are missing. Run scripts/fetch.sh, then scripts/patch.sh." >&2
    exit 1
}

if [[ -z "$ndk" || ! -d "$ndk" ]]; then
    echo 'Set ANDROID_NDK_ROOT (or ANDROID_NDK_HOME) to a valid Android NDK.' >&2
    exit 1
fi

toolchain="$ndk/toolchains/llvm/prebuilt/linux-x86_64"
[[ -d "$toolchain" ]] || {
    echo "Unsupported NDK host toolchain: $toolchain" >&2
    exit 1
}

cc="$toolchain/bin/aarch64-linux-android${android_api}-clang"
ar="$toolchain/bin/llvm-ar"
strip="$toolchain/bin/llvm-strip"
objcopy="$toolchain/bin/llvm-objcopy"
objdump="$toolchain/bin/llvm-objdump"

for tool in "$cc" "$ar" "$strip" "$objcopy" "$objdump" make python3; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "Missing required tool: $tool" >&2
        exit 1
    }
done

prefix="$build_dir/prefix"
proot_build="$build_dir/proot"
rm -rf "$build_dir"
mkdir -p "$prefix/include" "$prefix/lib" "$dist_dir"

cp "$package_dir/config/talloc-cross-answers.txt" "$talloc_source/cross-answers.txt"
(
    cd "$talloc_source"
    CC="$cc" AR="$ar" \
        ./configure --prefix="$prefix" --disable-rpath --disable-python \
        --cross-compile --cross-answers=cross-answers.txt
    make
    "$ar" rcs "$prefix/lib/libtalloc.a" bin/default/talloc*.o
    install -m 0644 talloc.h "$prefix/include/talloc.h"
)

make -C "$shmem_source" \
    CC="$cc" AR="$ar" \
    CFLAGS="-fPIC -std=c11 -Wall -Wextra -D__ANDROID_API__=$android_api" \
    libandroid-shmem.a
install -m 0644 "$shmem_source/libandroid-shmem.a" "$prefix/lib/libandroid-shmem.a"
install -Dm 0644 "$shmem_source/shm.h" "$prefix/include/sys/shm.h"

mkdir -p "$proot_build"
cp -a "$proot_source/." "$proot_build/"
(
    cd "$proot_build"
    make -C src \
        CC="$cc" AR="$ar" STRIP="$strip" OBJCOPY="$objcopy" OBJDUMP="$objdump" \
        CPPFLAGS="-I. -I$prefix/include -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE" \
        CFLAGS="-O2 -Wall -Wextra -DPROOT_UNBUNDLE_LOADER=\\\".\\\"" \
        LDFLAGS="-L$prefix/lib -ltalloc -landroid-shmem -llog -landroid -Wl,-z,noexecstack" \
        PROOT_WITH_LIBANDROID_SHMEM=true PROOT_UNBUNDLE_LOADER='.' proot
)

install -m 0755 "$proot_build/src/proot" "$dist_dir/proot"
install -m 0755 "$proot_build/src/loader/loader" "$dist_dir/loader"
"$strip" "$dist_dir/proot" "$dist_dir/loader"

echo "Built Android arm64-v8a PRoot in $dist_dir"
