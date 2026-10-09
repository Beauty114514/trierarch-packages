# Adreno Mesa package

This directory records the guest-side Mesa build used for the Adreno/Turnip
Wayland test. It is not part of the Android APK and it does not replace the
host Mesa installation.

The recipe is taken from the ifdevs Android-container Mesa package:

- repository: `https://github.com/lfdevs/archlinuxarm-PKGBUILDs.git`
- branch: `freedreno`
- Mesa source tag: `mesa-26.3.0-devel-20260824`
- Gallium drivers: `freedreno,zink,virgl,llvmpipe`
- Vulkan driver: `freedreno`
- Freedreno KMD: `kgsl`

The build script runs inside an ARM64 Arch guest as an ordinary user. It does
not modify the host or the Android app. Required build dependencies must be
installed in the guest before running it; `makepkg --nodeps` is used because
the package recipe is intended for the controlled container environment.

## Build

```bash
cd /tmp
git clone --depth=1 --branch freedreno \
  https://github.com/lfdevs/archlinuxarm-PKGBUILDs.git \
  trierarch-mesa/archlinuxarm-PKGBUILDs
cd trierarch-mesa/archlinuxarm-PKGBUILDs/extra/mesa
/path/to/trierarch-packages/mesa-adreno/scripts/build.sh
```

The five packages are written beside `PKGBUILD`:

```text
mesa-*.pkg.tar.xz
mesa-docs-*.pkg.tar.xz
vulkan-freedreno-*.pkg.tar.xz
vulkan-mesa-implicit-layers-*.pkg.tar.xz
vulkan-mesa-layers-*.pkg.tar.xz
```

## Install in the guest

Install all generated packages together so the Mesa and Vulkan components stay
in sync. For unsigned locally built packages, use a temporary pacman config;
do not weaken the permanent `/etc/pacman.conf`:

```bash
cp /etc/pacman.conf /tmp/pacman-nosig.conf
sed -i 's|^SigLevel.*|SigLevel = Never|' /tmp/pacman-nosig.conf
sudo pacman --config /tmp/pacman-nosig.conf -U --noconfirm \
  ./*.pkg.tar.xz
```

The package contains the upstream Zink single-physical-device fallback. No
KWin patch or Trierarch-specific Mesa source patch is required for this step.

## Verify

For Vulkan:

```bash
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/freedreno_icd.aarch64.json \
  vulkaninfo --summary
```

For OpenGL over Wayland through Zink/Turnip:

```bash
MESA_LOADER_DRIVER_OVERRIDE=zink \
GALLIUM_DRIVER=zink \
EGL_PLATFORM=wayland \
XDG_RUNTIME_DIR=/tmp/trierarch-wayland-user \
WAYLAND_DISPLAY=wayland-trierarch \
  eglinfo -B
```

The expected renderer contains `zink Vulkan ... Turnip Adreno`.
