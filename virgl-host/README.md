# Trierarch VirGL host

This package builds Trierarch's Android-side VirGL vtest server. A guest
container using Mesa's `virpipe` driver connects to that server over a Unix
socket; the server renders through Android EGL/GLES rather than guest
llvmpipe.

The package deliberately contains no app code. It only prepares native
artifacts under `dist/android/arm64-v8a`. `trierarch-app` packages them as
assets, starts the host through Android's linker, and currently bind-mounts
its vtest socket into DroidSpaces sessions.

## Build flow

```bash
cd virgl-host
bash scripts/fetch.sh
bash scripts/patch.sh

export ANDROID_NDK_HOME=/home/beauty/Android/Sdk/ndk/28.0.13004108
bash scripts/build-android.sh
```

`fetch.sh` creates ignored, checksum-verified upstream source trees.
`patch.sh` applies the checked-in Android compatibility patch series once.
`build-android.sh` builds only from that patched source tree.

## Artifacts

```text
dist/android/arm64-v8a/
├── virgl_test_server_android
├── virgl_render_server                 # Venus support, when produced
├── libvirglrenderer.so
├── libepoxy.so
└── angle/{vulkan,gl,vulkan-null}/*.so
```

The server is built with the old Trierarch configuration: surfaceless EGL,
GLES, and Venus render-server support. At runtime it must be launched through
Android's linker when extracted under an app-private `files/` directory,
because that mount is normally `noexec`.

## Guest contract

For a DroidSpaces profile with `graphics.renderer = "virgl"`, the app exposes
a per-session vtest socket and launches the guest with the equivalent of:

```bash
LIBGL_ALWAYS_SOFTWARE=1
GALLIUM_DRIVER=virpipe
VTEST_SOCKET_NAME=/path/to/vtest.sock
VTEST_RENDERER_SOCKET_NAME=/path/to/vtest.sock
```

This is Mesa's vtest/virpipe mode: `virpipe` sends Gallium rendering commands
to the host renderer over the vtest socket. `LIBGL_ALWAYS_SOFTWARE=1` selects
the software DRI frontend used by this mode; it does not mean the host renderer
falls back to guest-side llvmpipe. Mesa's documented vtest path reads rendered
results back and copies them to the client drawable, so this guest contract
does not provide direct dma-buf presentation to the Wayland compositor.

Do not set `MESA_LOADER_DRIVER_OVERRIDE=virpipe` here. That variable selects a
Mesa driver binary, while `virpipe` is the Gallium driver selected through
`GALLIUM_DRIVER`. Qt Quick can use its normal EGL/OpenGL integration; it does
not need a VirGL-specific Qt backend. `QT_QUICK_BACKEND=software`, when set in
the profile, is a separate Qt Quick choice and does not change KWin's renderer.
