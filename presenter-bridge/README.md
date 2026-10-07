# Trierarch presenter bridge

This package defines the local transport between the Android Surface presenter
and an opt-in guest compositor backend.  It deliberately contains no Wayland
global, Mesa patch, or desktop-specific behavior.

The lifecycle is:

1. Android dequeues a buffer from its own `ANativeWindow` queue.
2. The host announces the buffer's dma-buf to the guest backend with `BUFFER`.
3. The host asks for a frame with `RENDER`; its optional acquire fence is sent
   through `SCM_RIGHTS`.
4. The guest compositor renders into that same buffer and replies `FRAME_DONE`
   with its optional release `sync_file` fence.
5. Android queues the same native window buffer to SurfaceFlinger.

`slot_id` values are scoped to `generation`.  The host cancels a dequeued slot
if the backend disconnects, rejects the request, or fails to return it.  A
guest backend must drop every imported buffer on `RESET` or disconnect.

The Android host currently exposes this contract at
`$XDG_RUNTIME_DIR/trierarch-presenter.sock`.  It validates a `HELLO` handshake
only; Android buffer dequeue/queue and guest rendering are deliberately not
active yet, so existing SHM and linux-dmabuf rendering remain unchanged.
