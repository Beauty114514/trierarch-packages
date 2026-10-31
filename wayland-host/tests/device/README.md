# Rooted Adreno device tests

These tests target the rooted Adreno path only.  They are intentionally kept
outside `trierarch-app` so the release APK has no test UI.

## AHB/EGL

Build and run from a rooted device:

```bash
bash build.sh
bash run.sh
```

The executable allocates an `AHardwareBuffer`, fills it through the CPU,
imports it as an `EGLImage`, samples it with GLES2, and checks the readback.

## Scanout

`apk/` is a tiny test-only application.  Its `SurfaceView` gives the native
code a real Android `Surface`; the native code submits an AHB through
`ASurfaceControl` / `ASurfaceTransaction`.

```bash
bash apk/build_apk.sh
bash apk/run_scanout.sh
```

A successful run logs:

```text
transaction complete=true
scanout-result=0
```

The screenshot is written to `apk/build/trierarch-scanout.png`; the centered
256x256 surface should be red.
