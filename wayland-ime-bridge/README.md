# Trierarch Wayland IME bridge

The bridge is a guest client of the nested compositor's
`zwp_input_method_v1` and `zwp_input_method_v2`. It accepts complete UTF-8 text
commits from a local Unix stream socket and submits them with `commit_string`
when the compositor supplies a valid text-input state serial. KWin currently
uses v1, while wlroots-based compositors such as Hyprland use v2.

For KWin, Trierarch installs `trierarch-wayland-ime.desktop` into the guest's
applications directory and installs the bridge at
`/opt/trierarch/wayland-ime/trierarch-wayland-ime-bridge`. Select
**Trierarch Wayland Input Method** in **System Settings → Keyboard → Virtual
Keyboard**. KWin then starts the bridge as its input-method provider and passes
the authorized Wayland connection through `WAYLAND_SOCKET`; do not launch a
standalone bridge manually as an ordinary Wayland client for that KWin session.
This selection is stored by KWin and is not rewritten by Trierarch.

The KWin provider and wlroots client use the same Android-to-guest Unix socket:
`/tmp/trierarch-wayland-host/ime/trierarch-ime.sock`. KWin's provider uses the
inherited `WAYLAND_SOCKET`; wlroots sessions continue to connect through their
ordinary `WAYLAND_DISPLAY` socket.

It deliberately supports committed text only: it has no preedit support, Android
lifecycle integration, or socket authentication yet. On v2, Backspace, Delete,
Return, and Tab are translated to the corresponding text-input operations;
other keysyms remain available through the existing functional keyboard mode.

Build inside the guest:

```sh
bash scripts/build-linux.sh
```

For an arm64 cross-build on the development machine, fetch the pinned Debian
Wayland client sysroot first:

```sh
bash scripts/fetch-debian-arm64-sysroot.sh
CC=aarch64-linux-gnu-gcc bash scripts/build-linux.sh
```

Focus a native Wayland text field, then run in the same nested KWin session:

```sh
XDG_RUNTIME_DIR=/tmp/trierarch-wayland-user \
WAYLAND_DISPLAY=wayland-0 \
dist/trierarch-wayland-ime-bridge --commit 'hello from Trierarch'
```

The process reports the `commit_state` serial and exits after submitting the
text. Stop `input-method-probe` first: KWin permits one input-method client at
a time.

For the persistent bridge, pass `--socket`. Each message is one four-byte
network-order byte length followed by that many non-NUL UTF-8 bytes. The bridge
does not replace an existing socket path, and removes its own socket on normal
exit.

```sh
XDG_RUNTIME_DIR=/tmp/trierarch-wayland-user \
WAYLAND_DISPLAY=wayland-0 \
dist/trierarch-wayland-ime-bridge --socket /tmp/trierarch-wayland-user/trierarch-ime.sock
```

The Android-side lifecycle owner and client connection belong to the next step.
