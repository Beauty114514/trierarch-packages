# Trierarch session supervisor

`trierarch-session-supervisor` is a guest-side, non-invasive desktop-session
observer. It is not an `LD_PRELOAD` library and does not modify a compositor.

It records Wayland sockets discovered during a bounded startup observation
window. Its first adapter is deliberately narrow: when it finds a
`plasma_session` whose descendant `kwin_wayland` created a socket in the
observed runtime directory, it waits briefly for the normal Plasma startup.
Only when `plasmashell` is still absent does it copy that session's D-Bus/XDG
environment and launch `plasmashell --replace` on the KWin child socket.

It does not match X11 sessions, a standalone KWin, or non-Plasma Wayland
desktops. A shell it starts is stopped when the supervisor receives the parent
session's termination signal.

For chroot lifecycle ownership, the same executable has a dedicated watch
mode. It watches the owning Trierarch process's PID and start time, then
terminates the PTY session process group when that owner disappears:

```sh
dist/trierarch-session-supervisor \
  --owner-pid "$TRIERARCH_PID" \
  --owner-start-time "$TRIERARCH_START_TIME" \
  --pgid "$SESSION_PGID"
```

This mode does not inspect desktop processes, use cgroups, or scan unrelated
processes; it reads only the owner PID's `/proc/PID/stat` record to detect PID
reuse safely.

```sh
dist/trierarch-session-supervisor \
  --runtime-dir /tmp/trierarch-wayland-user \
  --log /tmp/trierarch-wayland-host/ime/trierarch-session-supervisor.log
```
