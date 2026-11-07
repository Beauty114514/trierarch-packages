# Trierarch Wayland host

Android-embedded Wayland compositor used by Trierarch to display a Linux
Wayland desktop on an Android `SurfaceView`.

The guest runs a nested compositor such as KWin or Hyprland. That compositor
connects to this host over a Unix socket; the host carries the resulting
buffers, cursor and input between the guest and Android. The guest remains
responsible for its own windows, decorations and effects. This package is the
display/input host, not a complete desktop shell.

```text
guest applications
        │
guest compositor (KWin / Hyprland / ...)
        │  Wayland socket
Trierarch Wayland host
        │  Vulkan or EGL on ANativeWindow
Android SurfaceView
```

## Status

The host is usable for the Trierarch Wayland desktop path and is under active
development. The Vulkan presenter is the primary Android renderer; EGL is the
compatibility fallback. Behaviour still depends on the guest compositor, GPU
driver and device, so treat this as an early-stage component rather than a
finished general-purpose compositor.

## What it implements

### Wayland server

The host advertises the interfaces a nested desktop needs:

- `wl_compositor`, `wl_subcompositor`, `wl_shm`
- `xdg_wm_base`, `xdg_output_manager`, `xdg_decoration_manager`
- `wl_output`, `wl_seat`, `wl_data_device_manager`
- `wp_viewporter`, `wp_single_pixel_buffer_manager`
- `wp_fractional_scale_manager`, `zwp_relative_pointer_manager`
- `zwp_pointer_constraints`
- `wp_presentation`
- `zwlr_layer_shell`
- `android_wlegl`
- `zwp_linux_dmabuf` v4, published only after the guest reports a usable
  dma-buf device

The data-device global is present, but clipboard and drag-and-drop are not a
complete feature yet.

### Rendering

The renderer accepts shared-memory buffers, dma-bufs and the legacy AHB/tvg
scanout path.

The Vulkan presenter:

- loads an external AdrenoTools/Turnip driver when configured;
- imports dma-bufs with their real pitch, offset and modifier instead of
  assuming tightly packed linear rows;
- caches imported images by dma-buf identity;
- composes the mapped surface tree, including subsurfaces, in stacking order;
- renders cursor surfaces with their hotspot;
- follows Android `SurfaceView` resizes;
- implements Android Vulkan pre-rotation with an identity-sized swapchain and
  a display-sized composition image;
- keeps host frames bounded with a `VkFence`, and releases guest buffers only
  after the host GPU is done with them.

If Vulkan cannot be configured, the EGL renderer provides the fallback path.
The EGL path uses different import and upload rules and is generally slower
than the Vulkan path.

### Input

The host implements pointer motion, buttons and scroll, keyboard events, focus
handling, pointer constraints and relative pointer input. Coordinates are
translated from the Android view into guest output space, including the
output fit/letterbox transform and Android orientation changes.

The external input helper and IME bridge live in their own packages:
`input-reader` and `wayland-ime-bridge`.

### Diagnostics

The host emits one-second renderer telemetry:

- commits, damage, repaint and render cadence;
- deferred buffer release depth and overflow;
- dmabuf import/cache and CPU-sync counters;
- Vulkan surface extent, transform, swapchain size and resize decisions.

These logs are intended for field diagnosis; they are not a stable API.

## Build

The package builds the pinned Wayland server and libffi, generates protocol
code, then builds the Android host with the NDK.

Requirements:

- Android NDK (the build defaults to the newest NDK under `$ANDROID_HOME/ndk`);
- Meson, Ninja, Make, `pkg-config` and `wayland-scanner`;
- the pinned upstream sources fetched by `scripts/fetch.sh`.

```bash
export ANDROID_NDK_HOME=/path/to/android-ndk
bash scripts/fetch.sh
bash scripts/build-android.sh
```

Artifacts are written to `dist/android/arm64-v8a/`:

```text
libtrierarch-wayland-host.so
libwayland-server.so
libffi.so
```

`tvgd` is a development-only token-service harness and is not part of the APK.

## Tests

Host-side protocol/server tests:

```bash
tests/host/run.sh
```

Device-only rooted Adreno tests live under `tests/device/`; they are kept out
of the release APK. See `tests/device/README.md` for the AHB/EGL and scanout
tests.

## Known limitations

- The guest compositor's output acquire fence is not fully solved. A compositor
  that presents a buffer without exposing a usable fence to this host can show
  tearing or flicker on fast-moving content. Desktop pixels that do not change
  can look stable while only the moving window flickers.
- EGL fallback does not have the same import, composition and synchronization
  behaviour as the Vulkan path.
- Clipboard and drag-and-drop are incomplete.
- GPU behaviour is device- and driver-dependent. Qualcomm/Adreno with a
  working Turnip build is the tested path; other GPUs fall back to the EGL or
  software paths.
- The host follows the guest compositor's output; it does not implement its own
  window management, panels or application launching.

## Upstream and ownership

Wayland, wayland-protocols, libffi and protocol XML are upstream material and
retain their licenses. Trierarch-owned compositor and renderer code lives under
`src/`. Exact pins and provenance are recorded in `UPSTREAM.md`; upstream
modifications must go through the documented patch workflow.

The Android app owns the `SurfaceView` lifecycle, JNI bridge, profiles and
user-facing UI. This package owns the native Wayland server, renderer and
input plumbing.
