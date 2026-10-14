#!/usr/bin/env bash
set -euo pipefail

printf '%s\n' '--- Vulkan ---'
if command -v vulkaninfo >/dev/null; then
    vulkaninfo --summary || true
else
    printf '%s\n' 'vulkaninfo is not installed.'
fi

printf '%s\n' '--- EGL ---'
if command -v eglinfo >/dev/null; then
    eglinfo -B || true
else
    printf '%s\n' 'eglinfo is not installed.'
fi
