# drm-lease-compat

Diagnostic and compatibility shims for compositors that call DRM lease APIs on
the PRoot fake render node.

## Current shim

`libdrm-lease-probe.so` intercepts `drmModeCreateLease` and appends one line to
`/tmp/trierarch-drm-lease-probe.log` for every call. It does not change the
function result; the real libdrm implementation is still called.

The log records:

- the DRM fd and its `/proc/self/fd` target;
- object count and flags;
- return value;
- `errno` after the call.

This is the first step for the Aquamarine PRoot investigation. A later shim may
translate an unsupported lease result into a fallback-friendly errno.

## Build

```sh
make
```

The default cross compiler is `aarch64-linux-gnu-gcc`; the output is:

```text
dist/linux/aarch64/libdrm-lease-probe.so
```

## Use

Copy the library into the PRoot guest and prepend it to `LD_PRELOAD`:

```sh
export LD_PRELOAD=/opt/trierarch/compat/libdrm-lease-probe.so
```

Then start the compositor and read:

```sh
cat /tmp/trierarch-drm-lease-probe.log
```
