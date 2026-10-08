#!/usr/bin/env sh
set -eu

protocol=/usr/share/wayland-protocols/stable/linux-dmabuf/linux-dmabuf-v1.xml
if [ ! -r "$protocol" ]; then
  printf '%s\n' "missing Wayland linux-dmabuf protocol XML: $protocol" >&2
  exit 1
fi

wayland-scanner client-header "$protocol" linux-dmabuf-v1-client-protocol.h
wayland-scanner private-code "$protocol" linux-dmabuf-v1-client-protocol.c
cc -O2 -Wall -Wextra -std=c11 \
  gpu_probe_vulkan_export.c vulkan_export.c wayland_dmabuf_client.c \
  linux-dmabuf-v1-client-protocol.c -I.. \
  $(pkg-config --cflags --libs vulkan wayland-client) -o gpu-probe-vulkan-export
