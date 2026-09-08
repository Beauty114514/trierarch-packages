# Trierarch KWin input-method probe

This is a diagnostic guest client, not an input method implementation. It binds
KWin's `zwp_input_method_v1` global and prints activation plus text-input state.
It never creates a surface and never sends `commit_string`, `preedit_string`,
or any other text injection request.

Build it inside an Arch guest with:

```sh
bash scripts/build-linux.sh
```

Run it in the nested KWin session:

```sh
XDG_RUNTIME_DIR=/tmp/trierarch-wayland-user \
WAYLAND_DISPLAY=wayland-0 \
dist/trierarch-inputmethod-probe
```

Focus a native Wayland text field. A working KWin input-method route emits
`activate`, `content_type`, `commit_state`, and, where the application supports
it, `surrounding_text`. Stop it with Ctrl-C before starting another input method.
