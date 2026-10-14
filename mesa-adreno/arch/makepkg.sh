#!/usr/bin/env bash
set -euo pipefail

if [[ "$(id -u)" -eq 0 ]]; then
    printf '%s\n' 'Run makepkg as the guest build user, not root.' >&2
    exit 1
fi

if [[ "${PWD##*/}" != mesa || ! -f PKGBUILD ]]; then
    printf '%s\n' 'Run this script from the ifdevs archlinuxarm-PKGBUILDs/extra/mesa directory.' >&2
    exit 1
fi

command -v makepkg >/dev/null || {
    printf '%s\n' 'makepkg is required.' >&2
    exit 1
}

# The Android-container recipe builds the documentation by default, but the
# docs package is optional for runtime testing and may require unavailable
# Sphinx extensions. Keep the runtime packages while skipping that target.
sed -i 's|html-docs=enabled|html-docs=disabled|' PKGBUILD
sed -i 's|    _pick docs usr/share/doc|    # _pick docs usr/share/doc|' PKGBUILD

makepkg --nodeps --noconfirm "$@"
