# Trierarch Wayland host

This package will provide Trierarch's Android-embedded Wayland compositor.
It is intentionally independent from `trierarch-app`: the package owns native
source preparation and Android artifacts, while the app owns the Java/Kotlin
bridge and UI integration.

The first milestone is deliberately small:

```text
Wayland client in a configured container
        ↓ wl_shm / xdg-shell
Trierarch Wayland host
        ↓ Android Surface
Trierarch app
```

The initial host will support only the minimum compositor path needed to
display one test client. Input, dmabuf, multi-window behavior, clipboard,
fractional scaling, and desktop-specific compatibility will be added in later
milestones.

## Current status

The first native compositor skeleton is now present under `src/`. It creates
one Wayland display and advertises `wl_compositor`, `wl_shm`, `wl_output`, and
`xdg_wm_base`. It accepts one fullscreen xdg-toplevel backed by an XRGB/ARGB
shared-memory buffer. There is still no Android `Surface` renderer or input
bridge; those are deliberately separate follow-up steps.

Prepare the pinned upstream sources with:

```bash
bash scripts/fetch.sh
```

The checkouts are generated under `sources/` and are ignored by Git. The
source-of-record for Trierarch changes will be checked-in patches and native
source under this package, not edits inside `sources/`.

Generate the protocol header after fetching:

```bash
bash scripts/generate-protocols.sh
```

This requires `wayland-scanner` on the build host. Generated files remain
ignored; reproducible builds regenerate them from the pinned checkout.

Build the current Android arm64 native artifacts with:

```bash
export ANDROID_NDK_HOME=/home/beauty/Android/Sdk/ndk/28.0.13004108
bash scripts/build-android.sh
```

The build compiles libffi and Wayland server with Meson, then uses
`ndk-build` for the Trierarch compositor. Artifacts are written to
`dist/android/arm64-v8a/`.

## Planned artifacts

The first native artifact is expected to be:

```text
dist/android/arm64-v8a/libwayland-compositor.so
```

The exact build script will be added after the minimum compositor source set
has been selected from the old project.

## Separation of responsibilities

- `wayland-host`: Wayland server library, compositor native code, protocol
  generation, and Android NDK build artifacts.
- `trierarch-app`: Surface lifecycle, JNI declarations, input forwarding, and
  user-facing configuration.
- container runtimes: launch the user's command with
  `XDG_RUNTIME_DIR` and `WAYLAND_DISPLAY`; they do not contain Trierarch UI
  logic.
