# Trierarch Wayland test clients

Small guest-side clients used to validate Trierarch's Wayland protocol
implementation without placing an intermediate compositor such as KWin between
the client and the Android host.  They are development tools, not runtime
components and are deliberately not packaged into the APK.

## Surface lifecycle probe

`trierarch-surface-lifecycle-probe` follows this exact sequence against the
socket selected by `WAYLAND_DISPLAY`:

```text
red wl_shm buffer -> wl_surface.attach(NULL) -> green wl_shm buffer
```

It keeps each state visible for the requested interval and prints each protocol
transition to standard output.  The NULL attach is intentionally between two
different live buffers, so it verifies that a compositor neither retains stale
content nor treats a detach as an empty commit.

Build natively inside a guest with Wayland client development files:

```sh
bash scripts/build-linux.sh
```

For an arm64 cross-build, reuse the Debian Wayland client sysroot already
prepared for `wayland-ime-bridge`:

```sh
CC=aarch64-linux-gnu-gcc bash scripts/build-linux.sh
```

Run it directly against the Trierarch parent socket, not KWin's child socket:

```sh
XDG_RUNTIME_DIR=/tmp/trierarch-wayland-user \
WAYLAND_DISPLAY=wayland-trierarch \
dist/trierarch-surface-lifecycle-probe --hold-ms 1500
```

The host should show red, blank, then green.  A later dma-buf probe will use
the same direct-parent model.
