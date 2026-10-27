# Trierarch Android PipeWire host runtime

Trierarch runs PipeWire, its PulseAudio-protocol server, WirePlumber, and a
separate PipeWire client that exposes an AAudio-backed `Audio/Sink` on Android.
Container clients connect through the native PipeWire socket or the
PulseAudio-compatible socket.

```text
guest audio client -> bind-mounted PipeWire/Pulse socket -> host PipeWire
                   -> standalone AAudio sink client -> Android AAudio
```

The initial runtime packages prebuilt Termux PipeWire 1.4.9-1 and WirePlumber
0.5.13 artifacts, plus an AAudio sink executable, from the pinned upstream
reference revision `0543368d3262e098b92ff0305e149ff0d1fce3e1`. This packaging
step does not patch or rebuild PipeWire.

## Prepare the Android runtime

```bash
cd trierarch-packages/pipewire-host
bash scripts/fetch.sh https://github.com/localdesktop/localdesktop.github.io.git
bash scripts/package-android.sh
```

The result is placed under `dist/android/arm64-v8a/` and consumed by the app's
asset packaging task. Generated source and binaries are ignored by Git. The
AAudio sink is an independent executable, not a PipeWire module. The fetch
script takes its source URL as an argument so upstream provenance remains
documented here rather than embedded in Trierarch's build code.

The runtime is currently enabled only for DroidSpaces profiles:

```toml
[audio]
backend = "pipewire"
```

The Android service creates both sockets before container startup and exposes
them through the session's `/tmp/trierarch-pipewire` bind mount. The first
guest-side verification is `pactl info`, followed by playback using `paplay`.

## Attribution and limits

The standalone AAudio sink and its reference implementation come from the
[Local Desktop project](https://github.com/localdesktop/localdesktop.github.io/tree/0543368d3262e098b92ff0305e149ff0d1fce3e1),
licensed under GPL-3.0. The bundle manifest records the pinned revision; source
attribution is kept here. PipeWire, WirePlumber, and the Termux-built runtime
artifacts retain their respective upstream licenses. This is an initial
playback implementation, not microphone capture or audio-focus support.

- [Reference design notes](https://localdesktop.github.io/docs/developer/pipewire-standalone-aaudio-poc/)
