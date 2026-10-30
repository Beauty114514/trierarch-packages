# Adreno Mesa package

This directory records the guest-side Mesa build used for the Adreno/Turnip
Wayland test. It is not part of the Android APK and it does not replace the
host Mesa installation.

The recipe is derived from the ifdevs Android-container Mesa work:

- packaging: `https://github.com/lfdevs/archlinuxarm-PKGBUILDs.git`,
  branch `freedreno`;
- Mesa fork: `https://github.com/lfdevs/mesa-for-android-container`,
  referenced by the packaging PKGBUILD;
- Gallium drivers: `freedreno,zink,virgl,llvmpipe`;
- Vulkan driver: `freedreno`;
- Freedreno KMD: `kgsl`.

This is a multi-layer downstream of Mesa, not a single “Mesa / MIT” component.
The current fork tag, packaging commit, license expression and the build
script's pinned default are recorded in [`UPSTREAM.md`](UPSTREAM.md). The build
uses `MESA_PKGBUILD_COMMIT` to override the pinned packaging commit; pin the
exact commit/tag before distributing binaries.

The build script runs inside an ARM64 Arch guest as an ordinary user. It does
not modify the host or the Android app. Required build dependencies must be
installed in the guest before running it; `makepkg --nodeps` is used because
the package recipe is intended for the controlled container environment.

## Build

The one-command entry point downloads the ifdevs PKGBUILD checkout into a local
`.build/` directory and then invokes the Mesa build:

```bash
cd /path/to/trierarch-packages/mesa-adreno
./arch/build.sh
```

The checkout location can be changed without modifying the repository:

```bash
TRIERARCH_MESA_BUILD_DIR=/tmp/trierarch-mesa ./arch/build.sh
```

The repository and branch are also overridable for testing:

```bash
MESA_PKGBUILD_REPO=https://github.com/lfdevs/archlinuxarm-PKGBUILDs.git \
MESA_PKGBUILD_BRANCH=freedreno \
./arch/build.sh
```

The lower-level two-command form remains available when the PKGBUILD checkout
has already been prepared:

```bash
cd /tmp
git clone --depth=1 --branch freedreno \
  https://github.com/lfdevs/archlinuxarm-PKGBUILDs.git \
  trierarch-mesa/archlinuxarm-PKGBUILDs
cd trierarch-mesa/archlinuxarm-PKGBUILDs/extra/mesa
/path/to/trierarch-packages/mesa-adreno/arch/makepkg.sh
```

## Build for Kali/Debian

Kali/Debian must use its own source package and Debian build rules. Do not
install Arch packages in a Debian-based guest. Inside the Kali guest, enable a
matching `deb-src` entry and install the Mesa build dependencies first, then
run:

```bash
cd /path/to/trierarch-packages/mesa-adreno
./debian/build.sh
```

The script runs `apt-get source mesa`, applies Trierarch's patches from
`debian/patches/`, and invokes `dpkg-buildpackage`. The generated `.deb`
packages are written beside the Debian source tree. Install them together:

```bash
./debian/install.sh /path/to/mesa-source-parent
```

The current patch queue targets Kali source package `mesa_26.1.6-1` (upstream
Mesa `26.1.6`). The script checks both version strings and stops instead of
silently applying the patches to another source revision. Updating Mesa
requires reviewing and retesting the patch queue first.

For an already prepared source tree:

```bash
MESA_DEBIAN_SOURCE_DIR=/tmp/mesa-<version> \
  ./debian/build.sh
```

The five packages are written beside `PKGBUILD`:

```text
mesa-*.pkg.tar.xz
mesa-docs-*.pkg.tar.xz
vulkan-freedreno-*.pkg.tar.xz
vulkan-mesa-implicit-layers-*.pkg.tar.xz
vulkan-mesa-layers-*.pkg.tar.xz
```

## Install in an Arch guest

Install all generated packages together so the Mesa and Vulkan components stay
in sync. For unsigned locally built packages, use a temporary pacman config;
do not weaken the permanent `/etc/pacman.conf`:

```bash
cp /etc/pacman.conf /tmp/pacman-nosig.conf
sed -i 's|^SigLevel.*|SigLevel = Never|' /tmp/pacman-nosig.conf
sudo pacman --config /tmp/pacman-nosig.conf -U --noconfirm \
  ./*.pkg.tar.xz
```

The Arch recipe contains the ifdevs downstream changes. The Debian recipe is
kept separate because it must apply its own patch queue to Kali's source
package.

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
