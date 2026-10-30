# Mesa / Adreno upstream chain

Trierarch's Adreno Mesa recipe is a downstream of Mesa and the ifdevs
Android-container work. Do not attribute the result as a single “Mesa / MIT”
component. The layers are:

```text
Mesa 3D upstream
  └── lfdevs/mesa-for-android-container (Freedreno/Android-container changes)
        └── lfdevs/archlinuxarm-PKGBUILDs, branch freedreno (PKGBUILD)
              └── trierarch-packages/mesa-adreno (build recipe / patches)
```

## Pinned references

| Layer | Reference | Notes |
| --- | --- | --- |
| Mesa 3D upstream | https://gitlab.freedesktop.org/mesa/mesa | Per-file licenses; package expression in the ifdevs PKGBUILD is `MIT AND BSD-3-Clause AND SGI-B-2.0` |
| lfdevs Mesa fork | https://github.com/lfdevs/mesa-for-android-container, tag `mesa-27.0.0-devel-20261025-r1` | Inspected 2026-10-30; the fork has Mesa's per-file license layout and no single root LICENSE |
| lfdevs packaging | https://github.com/lfdevs/archlinuxarm-PKGBUILDs, branch `freedreno`, commit `8a29bd2e748463663852eec750b4591a36728dae` | Inspected 2026-10-30; the repository has no top-level LICENSE; the Mesa PKGBUILD declares the license expression above |
| Trierarch Arch recipe | `arch/build.sh`, `arch/makepkg.sh`, `arch/install.sh` in this directory | Build scripts/recipes; packages repository GPL-3.0 unless a file says otherwise |
| Trierarch Debian recipe | `debian/build.sh`, `debian/patches/0001-trierarch-kgsl-wayland.patch`, `debian/patches/0002-trierarch-kgsl-build-option.patch` | Targets Kali source package `mesa_26.1.6-1`; the patch queue contains the ifdevs Freedreno changes plus Trierarch's KGSL Wayland/build changes |

The ifdevs `freedreno` branch moves. The Arch build script pins the default
packaging commit `8a29bd2e748463663852eec750b4591a36728dae`; set
`MESA_PKGBUILD_COMMIT` to override it. The branch name is only used to seed the
checkout. Before a release, record the exact ifdevs commit and Mesa/fork tag that
produced the binaries here. A branch name alone is not a reproducible provenance
record.

The README previously named a `mesa-26.3.0-devel-20260824` source tag. That was
a historical recipe reference; it is not the current ifdevs branch. Do not use
the old tag as a claim about the current build.

## Licenses

- **Mesa 3D / lfdevs fork**: per-file copyright and license notices are
  authoritative. The package expression commonly used by the ifdevs PKGBUILD is
  `MIT AND BSD-3-Clause AND SGI-B-2.0`; some files may carry other upstream
  notices.
- **lfdevs packaging**: no repository-level LICENSE was found. The PKGBUILD
  `license` field documents the packaged Mesa license; preserve maintainer and
  source headers.
- **Trierarch changes**: the packages repository is GPL-3.0. Trierarch-authored
  build scripts and patches are GPL-3.0 unless the file says otherwise.
  Trierarch does not relicense Mesa or the ifdevs contributions.

Mesa's permissive licenses allow redistribution of modified builds, but the
copyright and permission notices must be retained. Trierarch's GPL-3.0 patches
add the usual GPL source/notice obligations when a combined binary is shipped.
Do not describe a combined Trierarch Mesa build as “MIT-only”.

## Source availability

Corresponding source is available from:

- Mesa upstream at the exact source revision used by the fork/tag;
- the lfdevs Mesa fork at the recorded tag;
- the lfdevs packaging repository at the recorded commit;
- Trierarch's `mesa-adreno` build scripts and Debian patch queue in this
  repository;
- the rest of Trierarch's corresponding source in the app and packages
  repositories.

If you distribute a prebuilt Mesa package, ship the applicable Mesa/ifdevs
license notices and the corresponding Trierarch source/patches, or provide a
written offer valid under the applicable license.

This is a provenance record, not legal advice.
