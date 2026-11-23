# Trierarch Wayland IME bridge

The bridge has two modes. Its input-method mode is a guest client of the nested compositor's
`zwp_input_method_v1` and `zwp_input_method_v2`. It accepts complete UTF-8 text
commits from a local Unix stream socket and submits them with `commit_string`
when the compositor supplies a valid text-input state serial. KWin currently
uses v1, while wlroots-based compositors such as Hyprland use v2.

Its clipboard mode (`--clipboard`) watches the guest compositor's selection and
streams plain text to the Android app over a second Unix socket. It does not
bind an input-method object, so it keeps working when KWin rejects the
standalone IME instance.

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

The session wrappers install the bridge and start both modes. The Android app
connects to the IME socket for text input and to the clipboard socket through
`WaylandClipboardBridge`.

## Clipboard mode

```sh
XDG_RUNTIME_DIR=/tmp/trierarch-wayland-user \
WAYLAND_DISPLAY=wayland-1 \
dist/trierarch-wayland-ime-bridge --clipboard --socket /tmp/trierarch-wayland-host/ime/trierarch-clipboard.sock
```

The mode probes the registry and picks the first available clipboard protocol:

1. `ext_data_control_manager_v1` — KWin 6.5 and newer;
2. `zwlr_data_control_manager_v1` — wlroots compositors such as Hyprland;
3. `wl_data_device_manager` — fallback for compositors that deliver selection
   to ordinary clients.

The two data-control protocols are the ones meant for clipboard tools. A
background helper has no keyboard focus, and compositors such as KWin only
deliver `wl_data_device.selection` to focused clients, so the fallback is a
best-effort path rather than the normal one.

Accepted MIME types are `text/plain;charset=utf-8`, `text/plain`, and
`UTF8_STRING`; other charsets are ignored. Text is capped at 64 KiB and deduplicated. The
socket uses the same frame format as the IME socket: one four-byte network-order
length followed by that many UTF-8 bytes. The helper listens on the path, keeps
the latest text, and sends it to the Android app as soon as it connects.

The protocol XML files are vendored in `protocols/` and generated with
`wayland-scanner` during the build. The session wrappers start this mode next to
the IME bridge; it is not registered with KWin.

Limitations: plain text only; no images or files. On Android 13 and newer,
writing the clipboard from Linux shows the system clipboard preview.
