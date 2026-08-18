# Trierarch X11 host

This package builds the native Termux:X11 Lorie target for Trierarch. Its only
runtime artifact is:

```text
dist/android/arm64-v8a/libXlorie.so
```

It does not build an APK, a Termux companion package, or Android UI code.
The output also contains the XKB rules bundle required for Lorie to start on
plain Android, where no Linux rootfs has supplied `/usr/share/X11/xkb`.

## Build

The package uses Termux:X11's own CMake source through Android Gradle's
`externalNativeBuild` support. It requires Android NDK `29.0.14206865`,
Python 3, Bison, and a Java version supported by the pinned Android Gradle
Plugin.

```bash
./scripts/build-android.sh
```

The script invokes only `externalNativeBuildRelease` and copies both
`libXlorie.so` and the versioned XKB runtime asset to `dist/`.

The Android application is deliberately not part of this package. It will
later package this versioned native artifact and provide the Java/JNI display
bridge separately.
