#!/usr/bin/env bash
set -euo pipefail

packages=(
    meson ninja git
    python-mako python-packaging python-ply python-pycparser python-yaml
    wayland-protocols xorgproto
    clang libelf libx11 libxcb libxext libxshmfence libxxf86vm
    libdisplay-info libpng libva libxml2 libxrandr
    spirv-tools vulkan-icd-loader xcb-util-keysyms
)

if [[ $(uname -m) != "aarch64" ]]; then
    printf '%s\n' 'mesa-adreno: this dependency script is only for native aarch64 Arch Linux.' >&2
    exit 1
fi

if [[ ${1:---help} == "--print" ]]; then
    printf '%s\n' "${packages[@]}"
    exit 0
fi

if [[ ${1:---help} != "--install" ]]; then
    cat <<'EOF'
Usage: install-arch-aarch64.sh --install

Installs Mesa build dependencies only.  It does not install, replace, or
rebuild the mesa or vulkan-freedreno runtime packages.
EOF
    exit 64
fi

exec sudo pacman -S --needed "${packages[@]}"
