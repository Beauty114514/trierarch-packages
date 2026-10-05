# Trierarch GPU buffer probe

This package is an isolated feasibility probe.  It is not part of the Android
application build, the Wayland host, or the runtime launch path.

The protocol retains a guest-to-host direction and adds one equally narrow
reverse direction. The two guest samples exercise them separately:

```text
guest GBM/Mesa DMA-BUF
  -> SCM_RIGHTS
  -> Android EGL_EXT_image_dma_buf_import
  -> Android Surface texture draw

three Android AHardwareBuffer allocations
  -> runtime-inspected first native-handle FD + SCM_RIGHTS
  -> guest surfaceless EGL_EXT_image_dma_buf_import
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
- `guest/gpu_probe_guest.c` requests the reverse direction, imports the
  received Android pixel FD through a surfaceless EGL display, clears it cyan,
  exports an `EGL_ANDROID_native_fence_sync` release fence, and waits the host
  reuse fence before exit.
- `wayland-host/src/gpu_probe.c` allocates three 256×256 RGBA
  `AHardwareBuffer`, dynamically queries its non-NDK native handle, and sends
  only the first FD after logging its complete shape. `renderer.c` samples the
  original Android allocation as a temporary overlay after the guest replies.
- `guest/guest_to_host.c` exports one linear GBM DMA-BUF and sends it to the
  host. It is used to test direct Android EGL import and any experimental
  guest-DMA-BUF adapter without touching a Wayland surface.
- `guest/wayland_dmabuf.c` instead submits the same kind of GBM checkerboard
  as a `wl_buffer` through the standard `zwp_linux_dmabuf_v1` and `xdg-shell`
  protocols. It prints the exported geometry and waits briefly for a frame
  callback. This first-stage client does not change the host's normal buffer
  import path; host `dmabuf accepted` and render logs must be checked separately.
- For each guest-to-host packet, the host logs an allocator matrix under
  `TrierarchAdrenoAhb`: guest FD capacity and logical image size, then Android
  donor stride/capacity for RGBA and BGRA with sampled-only and renderable
  usage. `all_fields` reports whether the diagnostic calibration observed the
  expected metadata fields; it does not authorize importing or patching a
  guest buffer. Donor allocation failures are recorded and do not stop the
  existing import probe.
- If the guest FD capacity is an integral number of stride-sized rows beyond
  the visible height, the matrix also measures one RGBA sampled donor with
  that inferred allocation height. This only compares allocation geometry;
  it neither imports the padded donor nor changes the visible image size.
- The isolated guest-to-host probe also tries a padded donor only when its
  stride and pixel FD capacity exactly match the guest. It registers the guest
  FD with untouched Android donor metadata, records registration and EGLImage
  outcomes separately, and samples only the guest's visible rows. If it fails,
  the previous probe path remains available. Normal Wayland surfaces do not
  use this experiment. It still runs inside the app process; probe-only scope
  does not protect that process from a vendor graphics-driver crash.

The guest program reports the actual `GL_RENDERER`, surfaceless EGL extensions,
import result, GL error, frame rate, reuse-fence wait time, and both fence outcomes. The host reports the Android native-handle shape,
the assumed one-plane RGBA DRM metadata, and whether Android-side sampling
succeeded. A positive result means only that this particular Android allocator,
guest Mesa driver, and EGL import stack interoperate; it is not yet a general
buffer queue.

## Intentionally not implemented yet

A production queue policy, detailed fence telemetry, and any production protocol
belong to a later bridge. This fixed three-slot probe intentionally rejects a
native handle without a first FD, and does not claim that a multi-FD handle is
portable merely because this test can inspect it.

## Build

Neither build is wired into Gradle.

Guest, inside a test container with EGL/GLES development headers:

```sh
cc -O2 -Wall -Wextra -std=c11 guest/gpu_probe_guest.c -I. \
  $(pkg-config --cflags --libs egl glesv2) -o gpu-probe-guest
```

Guest-to-host import probe, cross-built with the same Debian arm64 sysroot
used by the other Trierarch test clients:

```sh
aarch64-linux-gnu-gcc -O2 -Wall -Wextra -Werror -std=c11 -I. \
  -I../wayland-ime-bridge/.sysroot/debian-bookworm-arm64/usr/include \
  -I../wayland-ime-bridge/.sysroot/debian-bookworm-arm64/usr/include/libdrm \
  guest/guest_to_host.c -L../wayland-ime-bridge/.sysroot/debian-bookworm-arm64/usr/lib/aarch64-linux-gnu \
  -Wl,-rpath-link,../wayland-ime-bridge/.sysroot/debian-bookworm-arm64/usr/lib/aarch64-linux-gnu \
  -lgbm -ldrm -o guest-to-host-probe
```

The Android listener is compiled by the normal `wayland-host` Android build.
It opens `gpu-probe.sock` alongside the host Wayland socket, but does no GPU
work unless a guest connects and sends a valid probe packet. It reuses the
renderer’s existing EGL context rather than creating a second context for the
Android Surface.

Standard Wayland DMA-BUF submission client, cross-built on the development
machine using the existing Debian arm64 sysroot:

```sh
bash scripts/build-wayland-dmabuf-arm64.sh
```

The output is `/tmp/trierarch-wayland-dmabuf-probe`. Run it as the guest user
against Trierarch's parent socket, not a nested compositor's `wayland-0`:

```sh
XDG_RUNTIME_DIR=/tmp/trierarch-wayland-user \
  /path/to/trierarch-wayland-dmabuf-probe wayland-trierarch 300 300 5
```

The final number holds the test surface for up to 5 seconds (1–30 allowed).
This can temporarily cover part of the desktop; it does not modify KWin or
the container. The client alone proves only standard-protocol submission and
callback delivery, not successful Android AHB import or GPU presentation.

### Native guest GLES submission probe

`guest/wayland_egl_probe.c` is deliberately different from the GBM
checkerboard client: Mesa owns the Wayland EGL surface and renders six GLES
frames with `eglSwapBuffers()`. It prints the actual guest `GL_RENDERER` and
therefore confirms which guest Mesa runtime created the submitted buffers.
It has no Trierarch-private protocol and is not packaged into the app.

Build it *inside the selected guest*, where that guest's private Mesa runtime
and Wayland/EGL development libraries are present:

```sh
bash scripts/build-wayland-egl-probe.sh
```

Then use the parent Trierarch socket (not KWin's `wayland-0`):

```sh
XDG_RUNTIME_DIR=/tmp/trierarch-wayland-user \
WAYLAND_DISPLAY=wayland-trierarch \
/tmp/trierarch-wayland-egl-probe
```

The guest output must identify the intended renderer, such as a KGSL-backed
Freedreno/Turnip path. Android logcat must then contain either `dmabuf
accepted` with FourCC, modifier, stride, offset and FD capacity, or no such
entry (meaning this guest EGL path selected SHM instead). Neither outcome
enables the experimental importer; this is only the stage-three contract
measurement.
