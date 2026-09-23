# Trierarch GPU buffer probe

This package is an isolated feasibility probe.  It is not part of the Android
application build, the Wayland host, or the runtime launch path.

The protocol retains the initial guest-to-host direction and adds one equally
narrow reverse direction. The current guest sample exercises the reverse one:

```text
guest EGL/Mesa texture
  -> EGL_MESA_image_dma_buf_export + SCM_RIGHTS
  -> Android EGL_EXT_image_dma_buf_import
  -> Android Surface texture draw

Android AHardwareBuffer allocation
  -> runtime-inspected first native-handle FD + SCM_RIGHTS
  -> guest surfaceless EGL_EXT_image_dma_buf_import
  -> guest GLES clear + native release fence
  -> Android waits that fence and samples its original allocation
  -> Android native read-complete fence -> guest waits before reuse
```

It answers a binary question that normal GL capability probes cannot answer:
can the Android host consume the *same* buffer that the guest Adreno Mesa
driver rendered into?

## Components

- `protocol.h` defines a small, versioned `SOCK_SEQPACKET` message.  It is not
  a proposed production protocol.
- `guest/gpu_probe_guest.c` requests the reverse direction, imports the
  received Android pixel FD through a surfaceless EGL display, clears it cyan,
  exports an `EGL_ANDROID_native_fence_sync` release fence, and waits the host
  reuse fence before exit.
- `wayland-host/src/gpu_probe.c` allocates a 256×256 RGBA
  `AHardwareBuffer`, dynamically queries its non-NDK native handle, and sends
  only the first FD after logging its complete shape. `renderer.c` samples the
  original Android allocation as a temporary overlay after the guest replies.

The guest program reports the actual `GL_RENDERER`, surfaceless EGL extensions,
import result, GL error, and both fence outcomes. The host reports the Android native-handle shape,
the assumed one-plane RGBA DRM metadata, and whether Android-side sampling
succeeded. A positive result means only that this particular Android allocator,
guest Mesa driver, and EGL import stack interoperate; it is not yet a general
buffer queue.

## Intentionally not implemented yet

A multi-buffer queue, fence timeouts/telemetry, and any production protocol
belong to a later bridge. The current implementation intentionally rejects a
native handle without a first FD, and does not claim that a multi-FD handle is
portable merely because this test can inspect it.

## Build

Neither build is wired into Gradle.

Guest, inside a test container with EGL/GLES development headers:

```sh
cc -O2 -Wall -Wextra -std=c11 guest/gpu_probe_guest.c -I. \
  $(pkg-config --cflags --libs egl glesv2) -o gpu-probe-guest
```

The Android listener is compiled by the normal `wayland-host` Android build.
It opens `gpu-probe.sock` alongside the host Wayland socket, but does no GPU
work unless a guest connects and sends a valid probe packet. It reuses the
renderer’s existing EGL context rather than creating a second context for the
Android Surface.
