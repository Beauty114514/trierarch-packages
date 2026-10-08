#!/usr/bin/env sh
set -eu

protocol=/usr/share/wayland-protocols/stable/linux-dmabuf/linux-dmabuf-v1.xml
xdg_protocol=/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
if [ ! -r "$protocol" ] || [ ! -r "$xdg_protocol" ]; then
  printf '%s\n' "missing Wayland client protocol XML" >&2
  exit 1
fi

wayland-scanner client-header "$protocol" linux-dmabuf-v1-client-protocol.h
wayland-scanner private-code "$protocol" linux-dmabuf-v1-client-protocol.c
wayland-scanner client-header "$xdg_protocol" xdg-shell-client-protocol.h
wayland-scanner private-code "$xdg_protocol" xdg-shell-client-protocol.c
cc -O2 -Wall -Wextra -std=c11 \
  gpu_probe_vulkan_export.c vulkan_export.c wayland_dmabuf_client.c \
  linux-dmabuf-v1-client-protocol.c xdg-shell-client-protocol.c -I.. \
  $(pkg-config --cflags --libs vulkan wayland-client) -o gpu-probe-vulkan-export
