#!/usr/bin/env bash
set -euo pipefail

package_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
binary="$package_dir/dist/device/arm64-v8a/trierarch-ahb-egl-test"
[[ -x "$binary" ]] || { echo "build the device test first: tests/device/build.sh" >&2; exit 1; }
adb wait-for-device
# The supported path is the rooted Adreno stack.  Use su directly because
# production builds commonly have adb root disabled while Magisk su works.
adb push "$binary" /data/local/tmp/trierarch-ahb-egl-test >/dev/null
adb shell chmod 755 /data/local/tmp/trierarch-ahb-egl-test
if adb shell 'su -c true' >/dev/null 2>&1; then
    adb shell su -c /data/local/tmp/trierarch-ahb-egl-test
else
    echo "su is not available; running as adb shell" >&2
    adb shell /data/local/tmp/trierarch-ahb-egl-test
fi
