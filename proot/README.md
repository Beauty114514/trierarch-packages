# Trierarch PRoot package

This is Trierarch's independently buildable Android packaging of
[Termux PRoot](https://github.com/termux/proot). It is not an Android Gradle
module and does not contain application code.

The Android arm64 build produces two executable ELF files:

```text
dist/android/arm64-v8a/proot
dist/android/arm64-v8a/loader
```

`proot` launches the traced guest process. `loader` must be deployed beside it:
PRoot uses that adjacent loader to enter guest ELF programs. The Android app will
consume a versioned package artifact; it must not build PRoot itself.

## Android app consumption

The package output location is part of the contract with `trierarch-app`:

```text
trierarch-packages/proot/dist/android/arm64-v8a/
├── proot
└── loader
```

When the app and packages repositories are checked out as sibling directories,
the app's Gradle build reads this location through:

```text
../trierarch-packages/proot/dist/android/arm64-v8a/
```

It packages the executable ELF files under Android-native names:

```text
lib/arm64-v8a/libproot.so
lib/arm64-v8a/libproot_loader.so
```

The `.so` suffix is only an APK-packaging convention; both files remain
executables. Android extracts them to the installed application's
`nativeLibraryDir`. Trierarch launches `libproot.so` directly and sets
`PROOT_LOADER` to the adjacent `libproot_loader.so`; users never copy either
file manually.

## Build flow

```text
scripts/fetch.sh → scripts/patch.sh → scripts/build-android.sh
```

Each step has one responsibility: fetch prepares pristine fixed sources, patch
applies only the declared Trierarch patches, and build compiles only already
patched sources. The build script never downloads or applies patches.

## Layout

```text
sources/         ignored pristine/upstream source worktree
patches/         Trierarch-owned patches and explicit application order
scripts/         fetch, patch, and Android build entry points
config/          checked-in cross-compilation answers
build/           ignored compiler intermediates and static libraries
dist/            ignored build outputs
```

## Scope

This package supplies the PRoot runtime only. It knows nothing about rootfs
downloads, archive extraction, profiles, terminal UI, or Android application
configuration.

See [UPSTREAM.md](UPSTREAM.md) for the fixed source revision and licensing.
