# Trierarch root input reader

This package contains the Android-native helper that lets a rooted Trierarch
installation receive physical keys before Android's `PhoneWindowManager`
consumes system shortcuts such as Meta/Super.

The helper:

- runs as root;
- finds external USB/Bluetooth keyboards by their evdev capabilities;
- takes an exclusive `EVIOCGRAB` on each matching device;
- streams `EV_KEY` events as fixed-size frames to stdout;
- watches the Trierarch process and exits when it disappears.

It never opens built-in buttons, touch panels, or virtual input devices.

## Frame format

All integers are little-endian `uint32_t`:

```text
magic ("TRKB" = 0x54524b42)
evdev key code
value (0 = release, 1 = press, 2 = repeat)
seconds
microseconds
```

The app forwards the evdev key directly to the Wayland host. This is the same
space used by `wl_keyboard`, so no Android key-code mapping is involved.

## Build

```sh
scripts/build-android.sh
```

The output is `dist/android/arm64-v8a/trierarch-input-reader`.

## Run

```sh
trierarch-input-reader --owner-pid <app-pid> --verbose
```

The helper is intended to be started by the app through `su`. It exits when the
owner process exits, and it releases all grabs when it shuts down.

## Scope

This solves system-reserved shortcuts on rooted devices. It does not replace
Android's soft keyboard or the normal `PhysicalKeyboardRouter`; those remain
available as a fallback when the helper cannot start.
