#!/usr/bin/env sh
set -eu
cc -O2 -Wall -Wextra -std=c11 gpu_probe_vulkan_export.c -I.. \
  $(pkg-config --cflags --libs vulkan) -o gpu-probe-vulkan-export
