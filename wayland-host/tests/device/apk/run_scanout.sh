#!/usr/bin/env bash
set -euo pipefail

package_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
apk="$package_dir/build/trierarch-devicetest.apk"
if [[ ! -f "$apk" ]]; then
    "$package_dir/build_apk.sh"
fi

adb wait-for-device
adb install -r "$apk" >/dev/null
adb shell am force-stop app.trierarch.devicetest
adb logcat -c
adb shell am start -n app.trierarch.devicetest/.MainActivity >/dev/null
sleep 3

log="$(adb logcat -d -s TrierarchScanoutTest:*)"
printf '%s\n' "$log"
printf '%s\n' "$log" | grep -q 'scanout-result=0'

mkdir -p "$package_dir/build"
adb exec-out screencap -p > "$package_dir/build/trierarch-scanout.png"
echo "scanout screenshot: $package_dir/build/trierarch-scanout.png"
