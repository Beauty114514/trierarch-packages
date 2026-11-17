# Single GBM device shim

`src/gbm_single_device.c` is an opt-in `LD_PRELOAD` library for PRoot
Hyprland on the Trierarch fake render node.

## Why

Aquamarine creates one GBM device for its output swapchain.  Hyprland then
creates a second GBM device for its own EGL fallback.  On the PRoot fake
render node (`/dev/kgsl-3d0` bound as `/dev/dri/renderD0`) the second
`gbm_create_device()` can block forever in Mesa/Turnip.

The shim makes the second creation reuse the first device.  It only changes
buffers allocated through that synthetic node; normal DRM sessions and other
compositors keep the real libgbm path.

## Build

```sh
make single
```

Output:

```text
dist/linux/aarch64/libgbm-single-device.so
```

## Use

The Trierarch app packages the library as an APK asset and deploys it to:

```text
/opt/trierarch/compat/libgbm-single-device.so
```

A PRoot Hyprland profile enables it explicitly:

```sh
export LD_PRELOAD=/opt/trierarch/compat/libgbm-single-device.so:$LD_PRELOAD
```

Preloading the library is the opt-in.  Set
`TRIERARCH_GBM_SINGLE_DEVICE=0` to disable it for a process that inherited
the preload.

Set `TRIERARCH_GBM_SINGLE_DEVICE_DEBUG=1` for one-line stderr diagnostics.

## Scope

- PRoot + Hyprland only;
- the shim only shares a GBM device for `/dev/dri/renderD0`;
- `gbm_device_destroy()` is reference-counted so either consumer can release
  its reference safely;
- it does not replace the real GBM allocator or the libdrm compatibility
  shims.
