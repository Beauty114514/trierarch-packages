# Upstream

This package builds the native Lorie source tree from
[Termux:X11](https://github.com/termux/termux-x11).

| Field | Value |
| --- | --- |
| Repository | `https://github.com/termux/termux-x11.git` |
| Pinned commit | `9471ad977d21b7c7bec45117008f74f330d45983` |
| Source preparation | `scripts/fetch-lorie.sh` |
| Local source location | `src/lorie/` (Git-ignored) |
| Native target | `Xlorie` → `libXlorie.so` |
| Required NDK | `29.0.14206865` |
| Upstream license | GPL-3.0-or-later; see upstream source notices |

Run `scripts/fetch-lorie.sh` before building. It checks out the pinned commit,
initializes the required submodules, then copies only `lorie/src/main/cpp/` to
`src/lorie/`. The resulting local source snapshot contains no nested Git
metadata and is not committed to this repository.

The package does not build or vendor `lorie-app`, `shell-loader`, or a Termux
companion package.

## Runtime XKB asset

`runtime-assets/lorie_xkb_bundled.zip` is the XKB rules data used by Lorie on
plain Android. It is installed beside `libXlorie.so` by `build-android.sh` and
then unpacked into Trierarch's private files directory by the application.

| Field | Value |
| --- | --- |
| Data source | X.Org/XKeyboardConfig data from the host `/usr/share/X11/xkb` tree |
| Archive root | `usr/share/X11/xkb/` |
| Consumer | `trierarch-app` → `X11Runtime` |
| Purpose | Supplies keyboard rules to Lorie on Android, where no Linux XKB tree is available |
| Redistribution | Upstream XKB data; retain the upstream notices and licensing when updating the archive |

The archive is committed because a clean checkout must be able to package the
same runtime asset without depending on the build host's installed XKB files.
It is separate from the Git-ignored upstream Lorie source snapshot and is not
an Android APK or a native library.
