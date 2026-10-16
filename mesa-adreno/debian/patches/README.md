# Debian Mesa patch queue

This directory is intentionally separate from Kali's own `debian/patches`.
`debian/build.sh` applies every `*.patch` here after obtaining Kali's Mesa
source package and before running `dpkg-buildpackage`.

Only patches tested against the pinned Kali Mesa source version belong here.
Do not copy Arch packages or Arch `PKGBUILD` files into a Debian guest.

The current `0001-trierarch-kgsl-wayland.patch` targets Kali source package
`mesa_26.1.6-1` (upstream Mesa `26.1.6`). It was regenerated from the Kali
chroot's actually working source tree, compared with a clean tree extracted by
`dpkg-source -x mesa_26.1.6-1.dsc`. The 21 affected source files were then
applied to that clean Debian baseline and compared byte-for-byte with the
working tree.

It contains the KGSL backend, the ifdevs Freedreno changes, and Trierarch's
Wayland EGL adjustment that selects `/dev/kgsl-3d0` when KGSL is requested
instead of trusting a display-controller `renderD128` advertised by dma-buf
feedback. It deliberately excludes generated build files, Debian packaging
outputs, the package-written `VERSION` file, and a no-op whitespace change.
