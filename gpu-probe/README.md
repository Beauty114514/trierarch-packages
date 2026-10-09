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

three Android AHardwareBuffer allocations
  -> AHardwareBuffer_sendHandleToUnixSocket() (opaque flattened handle + SCM_RIGHTS)
  -> transport-only validation in the guest
  -> guest renders up to three in-flight frames + native release fences
  -> Android waits the matching fence and samples each slot
  -> Android read-complete fence -> guest waits before reusing that slot
```

It answers a binary question that normal GL capability probes cannot answer:
can the Android host consume the *same* buffer that the guest Adreno Mesa
driver rendered into?

## Components

- `protocol.h` defines a small, versioned `SOCK_SEQPACKET` message.  It is not
  a proposed production protocol.
- `guest/gpu_probe_guest.c` requests the reverse direction. Its
  `--receive-only` mode validates the three opaque Android handle packets, all
  their received FDs, and their serialized sizes without attempting guest EGL
  import. The experimental render path remains for later work, but must not be
  interpreted as a general decoder for an Android GraphicBuffer handle.
- `wayland-host/src/gpu_probe.c` allocates three 256×256 RGBA
  `AHardwareBuffer` and transfers each complete handle through Android's public
  `AHardwareBuffer_sendHandleToUnixSocket()` API.
  `renderer.c` samples the original Android allocation as a temporary overlay
  after the guest replies.

### Full-size Wayland presenter gate

`guest/presenter_buffer_probe.c` is a separate one-shot client for the existing
`trierarch_adreno_presenter_v1` Wayland protocol. Unlike the socket transport
probe above, it asks the actual Wayland host for a buffer of a chosen size,
imports that buffer into guest EGL, clears it with Freedreno, checks a pixel
readback, submits a native fence, and attaches the returned `wl_buffer` on the
same Wayland connection. The host can then present that exact allocation via
its AHardwareBuffer path. It never starts KWin, changes Mesa, or runs unless
invoked explicitly.

Build from a checkout that contains both `gpu-probe` and sibling
`wayland-host/protocol`, inside the aarch64 guest with Wayland/EGL/GLES
development packages installed:

```sh
sh gpu-probe/guest/build-presenter-buffer.sh
XDG_RUNTIME_DIR=/tmp/trierarch-wayland-user \
WAYLAND_DISPLAY=wayland-trierarch \
  gpu-probe/guest/build/presenter-buffer/presenter-buffer-probe 1080 2244
```

The width and height should match the active host output. The probe requires
the host presenter global and native fence support; its failure must not be
treated as a reason to replace the working desktop path. Success proves only
that this device can render and present one host-owned allocation at that
size. It does not mean KWin has adopted host-owned buffers, or prove a public
DRM modifier for Android's opaque AHardwareBuffer layout.

The guest program reports the actual `GL_RENDERER`, surfaceless EGL extensions,
import result, GL error, frame rate, reuse-fence wait time, and both fence outcomes.
The transport gate reports every received FD and opaque serialized-payload
size. A positive result means only that this Android allocator can transfer a
complete handle over this Unix socket; it does not prove that glibc Mesa can
decode or import the handle as a Linux dma-buf.

## Intentionally not implemented yet

A production queue policy, detailed fence telemetry, and any production protocol
belong to a later bridge. This fixed three-slot probe accepts at most eight
received FDs and 4096 bytes of serialized handle payload. It intentionally does
not parse Android-private handle data.

## Build

Neither build is wired into Gradle.

Guest, inside a test container with EGL/GLES development headers:

```sh
cc -O2 -Wall -Wextra -std=c11 guest/gpu_probe_guest.c -I. \
  $(pkg-config --cflags --libs egl glesv2) -o gpu-probe-guest

# Transport-only gate; no EGL context or Mesa buffer import is attempted.
./gpu-probe-guest --receive-only /path/to/gpu-probe.sock
```

The Android listener is compiled by the normal `wayland-host` Android build.
It opens `gpu-probe.sock` alongside the host Wayland socket, but does no GPU
work unless a guest connects and sends a valid probe packet. It reuses the
renderer’s existing EGL context rather than creating a second context for the
Android Surface.
