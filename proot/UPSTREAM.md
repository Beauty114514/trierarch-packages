# Upstream

This package builds [Termux PRoot](https://github.com/termux/proot) from a
fixed upstream revision.

| Field | Value |
| --- | --- |
| Repository | `https://github.com/termux/proot.git` |
| Pinned commit | `a89b3732ec6ae1db674510f0843b2f3db54d0a2f` |
| Source preparation | `scripts/fetch.sh` |
| Local source location | `sources/proot/` (Git-ignored) |
| License | GPL-2.0-only; see upstream `COPYING` |

Run `scripts/fetch.sh` to obtain pristine fixed sources, then `scripts/patch.sh`
to apply Trierarch patches listed in `patches/series`. The resulting
`sources/` directory is local, contains no nested Git metadata, and is not
committed to this repository.

## Build dependencies

| Dependency | Fixed source | License | Use |
| --- | --- | --- | --- |
| talloc | 2.4.3, SHA-256 `dc46c40b9f46bb34dd97fe41f548b0e8b247b77a918576733c528e83abd854dd` | LGPL-3.0-or-later | static `libtalloc.a` |
| libandroid-shmem | 0.7, SHA-256 `1e5ff8459bc0a8c229dd8a94b27d119987e09ef3414331c2b5ebfff20b98e867` | BSD-3-Clause | static `libandroid-shmem.a` |

They are fetched by `scripts/fetch.sh` into ignored `sources/` paths and their
SHA-256 values are verified before extraction. They are not app runtime features
and are not embedded as source snapshots in this repository.

`patches/libandroid-shmem-0.7-android-ndk.patch` makes this dependency buildable
outside Termux's package environment: it adds the missing `fcntl.h` declaration and
defines Bionic's absent `_PATH_TMP` as `/tmp/`. No behavioral change is intended.

`patches/proot-a89b3732-android-ndk.patch` adds the missing `string.h` declaration
for the Android-only ashmem/memfd extension. It fixes strict Clang compilation only.
