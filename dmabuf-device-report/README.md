# Trierarch dma-buf device report

This small Linux guest companion reports the `dev_t` of its first visible
`/dev/dri/renderD*` node to the Android Wayland host. It deliberately does not
open the node, transfer buffers, or change Mesa/KWin configuration.

The report lets the host later publish truthful `linux-dmabuf-v1` v4 feedback
instead of advertising a fabricated `main_device = 0`.
