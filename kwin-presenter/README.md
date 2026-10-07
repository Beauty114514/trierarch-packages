# Trierarch KWin presenter backend

KWin output backends are compiled into the KWin binary.  This package therefore
contains an Arch-source overlay and patches, rather than an independently
loadable plugin.  It targets the currently tested KWin `6.7.5` source.

The first patch is deliberately a transport probe only:

```text
TRIERARCH_PRESENTER_PROBE_SOCKET=/tmp/.../trierarch-presenter.sock
  → KWin connects and sends HELLO
  → KWin continues using its ordinary Wayland backend
```

It does not create an output, import a buffer, modify the render loop, or
select the future Trierarch output backend.  Its only purpose is to verify
that the actual KWin process can use the shared presenter ABI before the
Output/EGL work begins.

Prepare an unpacked upstream KWin 6.7.5 source tree with:

```sh
bash arch/scripts/prepare-kwin-6.7.5-source.sh /path/to/kwin-6.7.5
```

The script copies the overlay and protocol header, then applies the patch.  It
fails without modifying the source further if the tree is not the expected
KWin layout.  Building or installing the patched KWin is intentionally a
separate, explicit step.

## Arch package build

For the tested Arch ARM KWin `6.7.5-1`, `arch/scripts/build-arch-package.sh`
creates an unsigned local `kwin-6.7.5-1.1-aarch64.pkg.tar.*` package with
`makepkg`.  It downloads the matching upstream source, resolves build
dependencies through normal Arch packaging, and does **not** install the
result.  Installation remains a separate recovery-sensitive action.
