# Android PipeWire host runtime

This package supplies the Android-side playback service used by DroidSpaces
profiles with `audio.backend = "pipewire"`. The app expects a bionic/arm64
runtime under `dist/android/arm64-v8a/` with this layout:

```text
bin/pipewire
bin/pipewire-pulse
etc/pipewire/pipewire.conf
etc/pipewire/pipewire-pulse.conf
lib/*.so
lib/pipewire-0.3/*.so
lib/spa-0.2/**/*.so
```

The payload must include an Android AAudio playback module and load it from
`pipewire-pulse.conf`. The minimum host service starts `pipewire` and
`pipewire-pulse`; it does not require WirePlumber for this PulseAudio playback
path. Host PipeWire and PulseAudio-protocol sockets are exposed to the
container at `/tmp/trierarch-pipewire/pipewire-0` and
`/tmp/trierarch-pipewire/pulse/native`; the app sets the matching guest
environment and bind-mounts that runtime directory when DroidSpaces starts.

The Android lifecycle and socket integration are in place, but this directory
does not yet contain the source-build/staging scripts or runtime artifacts.
Consequently PipeWire profiles fail with an explicit missing-payload error
until this package is built. No binaries are checked into Git.

Enable the backend only in a DroidSpaces profile:

```toml
[audio]
backend = "pipewire"
```

After a runtime is available, a first guest-side check is `pactl info` with
`PULSE_SERVER` set by Trierarch; the reported server should be PipeWire. Then
play a short WAV with `paplay` or `pw-play`. These runtime checks cannot pass
until the Android payload and AAudio module are built and packaged.

Implementation references:

- [Local Desktop PipeWire/AAudio architecture](https://localdesktop.github.io/docs/developer/pipewire-standalone-aaudio-poc/)
- [PipeWire AAudio/Oboe Android build](https://github.com/leefm0204/pipewire-termux-proot)
