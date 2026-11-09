# Staged KWin artifacts

This directory is the local staging area for built KWin compatibility
libraries. The binary itself is intentionally ignored by Git and is published
as a release asset.

`manifest.json` is tracked. It records the artifact identity, KWin package and
upstream versions, architecture, size, SHA-256 and build patch. Keep `url`
`null` until the artifact has been uploaded; do not point a manifest at a local
path.

To rebuild an artifact, use `../build-arch.sh` or `../build-debian.sh` against
the matching KWin source tree. Verify the resulting `libkwin.so.<version>`
contains only the render-node fallback before staging it here.
