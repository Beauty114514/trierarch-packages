# Trierarch session supervisor

`trierarch-session-supervisor` is a guest-side, non-invasive desktop-session
observer. It is not an `LD_PRELOAD` library and does not modify a compositor.

The first implementation only records Wayland sockets discovered during a
bounded startup observation window. Subsequent adapters may use the same
lifecycle and observation core to repair a known missing desktop component.

```sh
dist/trierarch-session-supervisor \
  --runtime-dir /tmp/trierarch-wayland-user \
  --log /tmp/trierarch-wayland-host/ime/trierarch-session-supervisor.log
```
