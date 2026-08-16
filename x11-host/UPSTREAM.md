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
