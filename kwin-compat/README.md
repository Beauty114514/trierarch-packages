# KWin compatibility patch

This directory carries the small KWin source patch used by Trierarch's nested
Wayland backend, plus version-agnostic build helpers.

It intentionally does **not** vendor KWin, Qt, KF6, or any distro dependency.
Build against the KWin source and dependency set of the target guest. A
patched `libkwin.so` is ABI-specific: do not copy an Arch artifact into a
Debian/Kali guest, or across different KWin releases.

## Patch

`patches/0001-wayland-render-node.patch` makes `WaylandBackend` fall back to an
explicit `KWIN_RENDER_NODES` entry when the host's dma-buf feedback names a
device that libdrm could not match inside the guest.

It is active only when both variables are set:

```text
TRIERARCH_NESTED_RENDER_NODE=1
KWIN_RENDER_NODES=/dev/dri/renderD0
```

The patch only changes `src/backends/wayland/wayland_backend.cpp`. It does
not alter normal KWin device selection when the feedback device is usable.

## Build

The source tree must already contain the KWin version matching the guest's
`kwin` package. On Arch:

```bash
cd trierarch-packages/kwin-compat
./build-arch.sh /path/to/kwin-6.8.2 /path/to/kwin-build 6
```

On Debian/Kali (with the matching source package unpacked):

```bash
./build-debian.sh /path/to/kwin-6.8.2 /path/to/kwin-build 6
```

Both wrappers call `build-source.sh`. The target `kwin` produces
`libkwin.so.<version>` under `build/bin/`.

Do not install a different distro's artifact. The library has C++ ABI and
Qt/KF6 dependencies tied to the guest's KWin package.

## Artifact naming

Use a name that records the ABI inputs, for example:

```text
libkwin-6.8.2-2-arch-aarch64.so
```

Record the distro package version, KWin source version, architecture and
SHA-256 in the release manifest. `versions.json` records tested combinations;
an absent combination is untested, not implicitly compatible.

## Runtime use

Keep the artifact outside the distro's package-managed `/usr/lib` and expose
it per session (bind mount/overlay). A package upgrade that replaces
`/usr/lib/libkwin.so.*` must not silently remove the compatibility path.
