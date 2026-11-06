# AdrenoTools Turnip runtime

Android arm64 `libadrenotools` and its hook libraries used by the host Vulkan
presenter.

The binaries are vendored from the pinned Turnip probe build so that ordinary
Gradle builds package them through `jniLibs`; they must not be injected into the
APK after the build.

```text
dist/android/arm64-v8a/
  libadrenotools.so
  libfile_redirect_hook.so
  libgsl_alloc_hook.so
  libhook_impl.so
  libmain_hook.so
```
