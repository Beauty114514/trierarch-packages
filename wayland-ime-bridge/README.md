# Trierarch Wayland IME bridge: first protocol milestone

This is a deliberately one-shot guest validation client. It connects to a
nested compositor's `zwp_input_method_v1`, waits until a text input is active,
and sends one UTF-8 `commit_string` before exiting.

It is **not** the eventual Android bridge: it has no Android socket, no
preedit support, no key forwarding, and no lifecycle integration. Its only
purpose is to prove that KWin accepts a real text commit from the protocol path
that Trierarch will later use.

Build inside the guest:

```sh
bash scripts/build-linux.sh
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
