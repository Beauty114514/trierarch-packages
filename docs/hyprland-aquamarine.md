# Hyprland / aquamarine

## Symptom

Hyprland starts inside the container, but the desktop is black. The host
compositor receives no frames:

```
TrierarchRenderer: perf ... commits=0 damage=0 renders=0
```

The guest Hyprland log shows it is using the aquamarine **headless** output
instead of the host Wayland output:

```
Output WAYLAND-1: initialized
backend: poll fd ... for implementation headless
CHeadlessOutput::scheduleFrame ...
```

The host output is present and visible to other clients:

```
interface: 'wl_output', version: 4
    name: Trierarch-1
```

So this is not a Trierarch host buffer, fence, or release problem. Hyprland is
not committing a surface to the host at all.

## Root cause

The distribution package `aquamarine 0.15.1-1` regresses nested Wayland output
handling: it creates a headless output instead of using the parent compositor's
`wl_output`.

The upstream fix is:

```
163b138 headless: reuse parent Wayland buffer formats when nested (#451)
```

The stock `0.15.1-1` package lacks this upstream behavior. The earlier
self-built `0.15.1-2` was newer than the stock package and did not show the
black screen; downgrading to `0.15.1-1` reintroduced it. No local Trierarch
patch is needed.

## Fix

If the distribution repository already has an aquamarine package containing the
fix, install that package. Otherwise build the latest upstream source and
install it over the distribution library.

Run inside the Arch guest as root:

```bash
pacman -S --needed --noconfirm cmake ninja git \
    hyprutils wayland wayland-protocols libdrm libinput seatd \
    libdisplay-info mesa libglvnd pixman

rm -rf /tmp/aquamarine /tmp/aquamarine-build
git clone --depth 1 https://github.com/hyprwm/aquamarine /tmp/aquamarine

cmake -S /tmp/aquamarine -B /tmp/aquamarine-build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr
cmake --build /tmp/aquamarine-build -j4
cmake --install /tmp/aquamarine-build
ldconfig
```

The current upstream repository uses CMake. Older tags used Meson; use the
build system present in the checkout.

Check that the installed library is active:

```bash
ldconfig -p | grep aquamarine
# libaquamarine.so.14 => /usr/lib/libaquamarine.so.14
```

`pacman -Q aquamarine` can still report the distribution version because the
library was installed directly with `cmake --install`, not through the package
manager. A later `pacman -Syu` may overwrite it; rerun this build after such an
upgrade.

## Verification

1. Restart Hyprland.
2. On the host, watch the renderer:

   ```bash
   adb logcat -d -s TrierarchRenderer | tail
   ```

   A working Hyprland produces non-zero counters:

   ```
   commits=42 damage=42 renders=42
   ```

3. In the guest Hyprland log, `CHeadlessOutput` should no longer be the active
   output; the host output `Trierarch-1` is used.

## Notes

- This affects Hyprland only. KWin/Plasma does not use aquamarine.
- `AQ_BACKENDS` is **not** a valid aquamarine variable; do not use it to force
  the backend. The supported variables are `AQ_TRACE`, `AQ_DRM_DEVICES`,
  `AQ_NO_MODIFIERS`, `AQ_MGPU_NO_EXPLICIT`, and `AQ_NO_ATOMIC`.
- If the picture appears but still flickers, that is a separate issue in the
  adreno/`android_wlegl` path: the guest buffer has no acquire fence. Fixing the
  black screen does not add that fence.
