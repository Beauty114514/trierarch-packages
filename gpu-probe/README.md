# Trierarch GPU buffer probe

This package is an isolated feasibility probe.  It is not part of the Android
application build, the Wayland host, or the runtime launch path.

The first probe direction is deliberately narrow:

```text
guest EGL/Mesa texture
  -> EGL_MESA_image_dma_buf_export + SCM_RIGHTS
  -> Android EGL_EXT_image_dma_buf_import
  -> Android Surface texture draw
```

It answers a binary question that normal GL capability probes cannot answer:
can the Android host consume the *same* buffer that the guest Adreno Mesa
driver rendered into?

## Components

- `protocol.h` defines a small, versioned `SOCK_SEQPACKET` message.  It is not
  a proposed production protocol.
- `guest/gpu_probe_guest.c` creates an off-screen GLES texture, fills it with
  a distinctive colour, and tries to export it using
  `EGL_MESA_image_dma_buf_export`.
- `wayland-host/src/gpu_probe.c` accepts the exported FD through the existing
  Wayland event loop.  `renderer.c` imports it as an EGLImage with the active
  renderer context and draws one temporary overlay.

The guest program reports the actual `GL_RENDERER`, EGL extensions, exported
DRM format, stride, modifier, and EGL error.  The host reports its EGL
extensions and whether `eglCreateImageKHR` and texture sampling succeeded.

## Intentionally not implemented yet

The reverse Android `AHardwareBuffer` -> guest direction is not represented as
one generic dma-buf plane.  Android native handles may carry multiple FDs and
private integer metadata; treating them as ordinary DRM dma-bufs would make a
false probe.  It will be added only after this direction reports the exact
guest-importable handle shape.

Likewise, acquire/release fences and buffer queues belong to the later bridge,
not this single-buffer feasibility test.

## Build

Neither build is wired into Gradle.

Guest, inside a test container with EGL/GLES development headers:

```sh
cc -O2 -Wall -Wextra -std=c11 guest/gpu_probe_guest.c -I. \
  $(pkg-config --cflags --libs egl glesv2) -o gpu-probe-guest
```

The Android listener is compiled by the normal `wayland-host` Android build.
It opens `gpu-probe.sock` alongside the host Wayland socket, but does no GPU
work unless a guest connects and sends a valid probe packet.  It reuses the
renderer's existing EGL context rather than creating a second context for the
Android Surface.
