# Trierarch Wayland IME bridge

The bridge is a guest client of the nested compositor's
`zwp_input_method_v1` and `zwp_input_method_v2`. It accepts complete UTF-8 text
commits from a local Unix stream socket and submits them with `commit_string`
when the compositor supplies a valid text-input state serial. KWin currently
uses v1, while wlroots-based compositors such as Hyprland use v2.

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
