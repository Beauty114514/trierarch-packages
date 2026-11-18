# Aquamarine PRoot compatibility shim

`src/aquamarine_compat.c` is a single opt-in `LD_PRELOAD` library for PRoot
Hyprland on the Trierarch fake render node.

It combines the five compatibility fixes that a normal DRM/GBM device would
provide:

- `drmGetDeviceNameFromFd2()` returns the synthetic node name;
- `drmGetCap(DRM_CAP_PRIME)` reports import/export capability;
- `drmModeCreateLease()` reports `EOPNOTSUPP` so Aquamarine can fall back;
- a second `gbm_create_device()` reuses the first GBM device instead of
  deadlocking;
- `drmPrimeFDToHandle()` and `drmCloseBufferHandle()` can be stubbed for the
  synthetic node, so Hyprland's LinuxDMABUF precheck does not reject a buffer
  before its EGLImage import path runs.

Every other fd keeps the real libdrm/libgbm behaviour, so rooted DRM sessions
and other compositors are unaffected.

## Build

```sh
make combined
```

Output:

```text
dist/linux/aarch64/libaquamarine-compat.so
```

## Install

Copy the library into the guest:

```sh
install -d /opt/trierarch/compat
install -m 755 libaquamarine-compat.so /opt/trierarch/compat/
```

Enable it only in the PRoot Hyprland profile:

```sh
export TRIERARCH_FAKE_PRIME_IMPORT=1
export LD_PRELOAD=/opt/trierarch/compat/libaquamarine-compat.so:$LD_PRELOAD
```

`TRIERARCH_FAKE_PRIME_IMPORT` is the opt-in for the PRIME validation stub.
Without it, only the device/lease/GBM compatibility paths are active.

The shim is active when preloaded.  Set
`TRIERARCH_AQUAMARINE_COMPAT_DEBUG=1` for one-line stderr diagnostics.

## Scope

- PRoot Hyprland only;
- only acts when the fd target is `/dev/dri/renderD0`;
- `gbm_device_destroy()` is reference-counted;
- it does not replace the real GPU driver or Mesa; it only answers Aquamarine's
  device/lease/PRIME probes and shares the GBM device;
- `TRIERARCH_FAKE_PRIME_IMPORT=1` only affects Hyprland's LinuxDMABUF validation
  on `/dev/dri/renderD0`; the actual buffer import is still done by Hyprland's
  EGLImage path.
