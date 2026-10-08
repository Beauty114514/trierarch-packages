# Arch Linux ARM Venus package overlay

This overlay adds Mesa's standard `vulkan-virtio` package to the exact Arch
Linux ARM Mesa recipe used by `mesa-for-android-container`.  It is not a
private Mesa bundle and must not be activated with `LD_LIBRARY_PATH`.

The produced package installs through `pacman -U` alongside the guest's normal
Mesa package set:

```text
/usr/lib/libvulkan_virtio.so
/usr/share/vulkan/icd.d/virtio_icd.aarch64.json
```

With `VN_DEBUG=vtest` and Trierarch's per-session vtest socket, this is the
guest Venus Vulkan ICD.  It is deliberately a separate verification step from
Wayland presentation: a successful `vulkaninfo` only proves command transport.

## Locked input

The patch targets `lfdevs/archlinuxarm-PKGBUILDs`, branch `freedreno`, commit
`6d69dfe57b780f2c921ec3991bc681349604fa69` (`extra/mesa`, version
`1:26.3.0-1`).  That matches the currently tested Arch guest Mesa package
version.  It also locks the Mesa-required `venus-protocol` source to
`e94b12f301b9eb27ebead757128a18420b4f7994` and exposes it to Meson as the
declared `subprojects/venus-protocol-1.1` fallback.

## Build procedure

Perform the build as an ordinary, non-root Arch guest user.  The resulting
packages are installed separately and explicitly by an administrator; this
repository never deploys or auto-installs them.

```sh
git clone --depth 1 --branch freedreno \
  https://github.com/lfdevs/archlinuxarm-PKGBUILDs.git archlinuxarm-pkgbuilds
cd archlinuxarm-pkgbuilds
git apply /path/to/0001-arch-arm-add-vulkan-virtio.patch
cd extra/mesa
makepkg -s
sudo pacman -U ./vulkan-virtio-*.pkg.tar.*
```

Before the install step, inspect the package with
`scripts/verify-package.sh`; it must contain both the Virtio ICD JSON and the
`libvulkan_virtio.so` library.  Do not replace the guest's entire Mesa set just
to test Venus.

The normal online build lets makepkg retrieve the two locked VCS sources.  For
an offline/reproducible build, preseed both bare repositories and use
`makepkg --holdver`; do not disable Meson's `nodownload` policy.

## Scope

The overlay does not alter KWin, the Android Wayland host, Mesa's Freedreno
configuration, or any app profile.  It only supplies the missing guest Vulkan
ICD in the same package-manager model as the distribution.
