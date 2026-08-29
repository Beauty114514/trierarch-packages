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
GALLIUM_DRIVER=virpipe
MESA_LOADER_DRIVER_OVERRIDE=virpipe
LIBGL_ALWAYS_SOFTWARE=0
VTEST_SOCKET_NAME=/path/to/vtest.sock
VTEST_RENDERER_SOCKET_NAME=/path/to/vtest.sock
```

Qt Quick then sees a normal EGL/OpenGL implementation. It does not need a
VirGL-specific Qt backend.
