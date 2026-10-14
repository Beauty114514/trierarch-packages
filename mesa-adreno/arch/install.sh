#!/usr/bin/env bash
set -euo pipefail

if [[ "$(id -u)" -eq 0 ]]; then
    printf '%s\n' 'Run pacman as the guest user; sudo is invoked by this script.' >&2
fi

shopt -s nullglob
packages=(./*.pkg.tar.*)
if ((${#packages[@]} == 0)); then
    printf '%s\n' 'No Arch Mesa packages found in the current directory.' >&2
    exit 1
fi

exec sudo pacman -U --noconfirm "${packages[@]}"
