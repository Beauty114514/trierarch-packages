# Trierarch Adreno Mesa bundles

This directory contains the reproducibility contract and per-distribution
packaging entry points for Trierarch's experimental private Adreno Mesa
runtime.  It is intentionally separate from the Android host and from guest
system packages.

The eventual runtime layout is:

```text
/opt/trierarch/mesa/adreno/<bundle-id>/
├── manifest.toml
├── lib/
└── lib/dri/
```

The common bridge and Mesa patch will be shared by every bundle.  A target
script is only responsible for the distribution's build dependencies and
installation layout.  Therefore a profile can select `renderer = "adreno"`
without containing paths or distribution-specific environment variables.

## Source lock before patches

`sources.lock.toml` locks the validated guest runtime
`mesa-for-android-container_26.3.0-devel-20260824_archlinux_arm64.tar` to its
upstream release tag, exact source commit, and published archive SHA-256.
This is intentionally not the nearby stock Mesa 26.2.3 checkout or the moving
`adreno-main` branch.  Every build entry point must first run
`scripts/verify-source-lock.sh`; it rejects an incomplete or malformed lock.

`scripts/prepare-source.sh --prepare` then clones/fetches only that locked tag
into the ignored `cache/` directory and creates a detached source worktree in
the ignored `work/` directory.  It refuses a moved tag or pre-existing
worktree with a different commit.  Preparing sources does not apply patches or
compile anything.  Apply the tracked patch series explicitly with
`scripts/apply-patches.sh <worktree>`; this makes an experimental bundle's
Mesa source changes reproducible rather than leaving untracked edits in
`work/`.

## Arch Linux ARM contract

`targets/arch-aarch64.toml` defines the first bundle boundary from the locked
Arch Linux ARM release.  It intentionally includes Mesa implementation
libraries, KGSL/Freedreno drivers, and the Freedreno Vulkan ICD, while leaving
the guest distribution's libglvnd dispatch libraries in place.  This lets the
renderer select a private implementation without replacing `/usr/lib`.

Use `targets/arch-aarch64.sh --inspect-release <archive>` to verify a release
archive against both the source lock and this file contract.  It only reads the
archive; it does not extract it into a guest rootfs or install packages.

The target profile also records the public KGSL/Freedreno Meson baseline from
the upstream project's current development guide.  That guide was added after
the locked release tag, so the fields are explicitly a reference rather than a
claim that the historical release used byte-identical options.  The first
native Arch ARM build must validate the list with `meson setup` before any
Trierarch patch is applied.

`targets/arch-aarch64.sh --configure <source> <build> <prefix>` performs that
validation on a native Arch aarch64 guest.  It refuses an existing build
directory, invokes only `meson setup`, and installs nothing.  `ninja` remains
a later, separately authorized step.

After an authorized `ninja`, use `targets/arch-aarch64.sh --stage <build>
<bundle-id> <stage-dir>` to produce a disposable staging tree.  The build must
already have been configured with the exact prefix
`/opt/trierarch/mesa/adreno/<bundle-id>`; staging refuses an inconsistent
prefix.  It writes `manifest.toml` and `files.sha256` into the staged bundle.

`scripts/deploy-private-bundle.sh <stage-dir> <bundle-id>` is the only step
that copies a bundle to `/opt/trierarch/mesa/adreno/<bundle-id>`.  It refuses
an existing destination, verifies every staged file before the final rename,
and does not change `current` by default.  Passing `--activate` atomically
changes only this stable selector.  This is also how an already installed
bundle is activated: it is re-verified, never overwritten.

```text
/opt/trierarch/mesa/adreno/current -> <bundle-id>
```

The future `renderer = "adreno"` runtime will resolve that selector, not a
versioned path compiled into the Android application or native launcher.  A
failed candidate can remain installed but inactive, or `current` can be
switched back without touching `/usr` or the system Mesa packages.

Before that validation, run
`dependencies/install-arch-aarch64.sh --install` in the native Arch guest.
It installs only build tools and headers; it never installs or replaces the
guest `mesa` or `vulkan-freedreno` packages.

## Targets

Only `arch-aarch64` is planned first because the validated runtime artifact is
an Arch Linux ARM package.  Debian and Ubuntu targets must be added only after
their own dependency and ABI contracts are verified.  No target script may
install or replace the guest distribution's system Mesa packages.

## Bundle manifest contract

Every produced bundle must carry the fields described in
`manifests/bundle.schema.toml`: distro identity, architecture, glibc ABI, Mesa
source revision, bridge ABI, and file hashes.  Runtime selection will reject a
mismatched bundle and use the existing renderer fallback instead.

This directory is a build/distribution scaffold only.  It does not yet hook
Mesa, modify KWin, or change an active renderer path.
