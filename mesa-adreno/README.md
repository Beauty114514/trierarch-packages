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

`sources.lock.toml` is deliberately unresolved at this stage.  The currently
tested guest runtime identifies itself as
`mesa-for-android-container_26.3.0-devel-20260824_archlinux_arm64.tar`, but
the corresponding source revision and archive hash have not yet been proven.
Applying a patch to the nearby stock Mesa 26.2.3 checkout, or to the moving
`adreno-main` branch, could produce a binary incompatible with that runtime.

Every build entry point must first run `scripts/verify-source-lock.sh`.  It
refuses to build until the exact upstream revision and source archive SHA-256
are recorded.  Resolving that provenance is the next task; it is not optional
metadata.

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
