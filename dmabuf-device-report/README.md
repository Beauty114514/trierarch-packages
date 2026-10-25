# Trierarch dma-buf device report

This small Linux guest companion reports the `dev_t` of the first accessible
`/dev/dri/renderD*` node to the Android Wayland host. It opens each candidate
before reporting it and deliberately does not transfer buffers or change
Mesa/KWin configuration. The host acknowledges the report after registering
it; the guest waits for that acknowledgement before launching the desktop, so
KWin cannot race the asynchronous host event loop.

The report lets the host later publish truthful `linux-dmabuf-v1` v4 feedback
instead of advertising a fabricated `main_device = 0`.
