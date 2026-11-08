# Trierarch session supervisor

`trierarch-session-supervisor` is a guest-side, non-invasive desktop-session
observer. It is not an `LD_PRELOAD` library and does not modify a compositor.

It discovers a running Plasma Wayland session, then keeps watching it instead
of exiting after a fixed startup window. Its first adapter is deliberately
narrow: when it finds a `plasma_session` whose descendant `kwin_wayland` created
a socket in the observed runtime directory, it waits for the normal Plasma
startup. If `plasmashell` never appears on a session without a systemd user
manager, it copies that session's D-Bus/XDG environment and launches
`plasmashell --replace` on the KWin child socket.

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

## Plasma Wayland mode

```sh
dist/trierarch-session-supervisor \
  --runtime-dir /tmp/trierarch-wayland-user \
  --log /tmp/trierarch-wayland-host/ime/trierarch-session-supervisor.log \
  --status-file /tmp/trierarch-wayland-host/ime/session-supervisor.json
```

Options:

- `--observe-ms` discovery window (default 10000); after discovery the
  supervisor keeps running until the session ends.
- `--grace-ms` how long to wait for `plasmashell` before fallback (default
  8000).
- `--fallback auto|always|off` (default `auto`). `auto` leaves the lifecycle to
  startplasma when a systemd user manager is available, and falls back when it
  is not.
- `--dry-run` logs what would be launched without starting a process.
- `--status-file` writes an atomic JSON snapshot for diagnostics.

It never uses the inherited host `WAYLAND_DISPLAY`; the fallback always uses
the socket created by the nested KWin process. It does not kill a
`plasmashell` started by startplasma.

## Limitations

- Plasma Wayland only; X11 and standalone KWin are out of scope.
- Fallback is limited to a bounded number of attempts with exponential backoff.
- The supervisor observes `/proc`; it does not inject code into the session.
