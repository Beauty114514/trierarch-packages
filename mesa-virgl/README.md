# mesa-virgl（POC）

“最后一格”的第一步：让 **virpipe/vtest** 接受合成器的 `EGL_LINUX_DMA_BUF_EXT`
图像，从而让未修改的 KWin 6.8 至少能走到“创建 swapchain 图像”这一步。

> 状态：仅 POC，不在产品默认路径里。补丁完成后图像只是普通的 virgl 纹理；
> 宿主还不会把它 blit 到 AHB 显示（那是下一步 token/scanout）。

## 为什么需要这个补丁

KWin 的 `EglSwapchain` 会对 GBM buffer 调用：

```
eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT, fd, ...)
```

Mesa 的路径：

```
dri_create_image_from_winsys()
  → pscreen->resource_from_handle()
    → virgl_resource_from_handle()
      → vws->resource_create_from_handle()
        → vws->resource_get_storage_size()
```

而 **vtest winsys 没有实现这两个函数指针**（只有 DRM winsys 有），所以
virpipe 下会在 `eglCreateImageKHR` 处解引用 NULL → 崩溃/失败。

另外，virgl 的 `pipe_screen` 没有设置 `caps.dmabuf`，Mesa 因此不公布
`EGL_EXT_image_dma_buf_import`，KWin 可能连扩展检查都过不去。

补丁做两件事：

1. `virgl_screen.c`：通过 `pipe_caps *` 写入 `dmabuf |= DRM_PRIME_CAP_IMPORT`
   （Mesa 27 的 `pipe_screen::caps` 是 `const`，不能直接赋值），公布导入扩展；
2. `virgl_vtest_winsys.c`：实现 `resource_create_from_handle`（忽略 fd 内容，
   按 `templ` 创建普通 virgl 纹理）和 `resource_get_storage_size`。

已验证补丁可干净应用到 **Mesa 26.2.5**、**26.1.6**（后者 7 行 offset）和
**27.0.0-devel**（当前 Arch 容器，offset 较大但 hunk 成功）。

## POC 实测（Arch Linux ARM / Mesa 27.0.0）

在 DroidSpaces Arch 容器里用最小配置构建（只保留验证需要的驱动）：

```bash
meson setup build --prefix=/tmp/mesa-install --buildtype=release \
  -Dgallium-drivers=freedreno,llvmpipe,virgl -Dfreedreno-kmds=kgsl \
  -Dvulkan-drivers= -Dglx=disabled -Dglvnd=disabled \
  -Degl=enabled -Dgles2=enabled -Dgbm=enabled -Dplatforms= \
  -Dvideo-codecs= -Dgallium-va=disabled -Dhtml-docs=disabled \
  -Dlibunwind=disabled -Db_ndebug=true
ninja -C build install
```

需要 `freedreno`/`kgsl`，否则 GBM 平台初始化会报 `kmsro: driver missing`
（真实 DRM 节点是 msm）。

vtest 服务端运行后（会话自带的，或按下面命令手动起）：

```bash
run-as app.trierarch env \
  LD_LIBRARY_PATH=.../virgl/lib:.../virgl/bin:.../virgl/angle/vulkan:.../lib/arm64:/system/lib64 \
  ANGLE_LIBS_DIR=.../virgl/angle/vulkan \
  XDG_RUNTIME_DIR=.../virgl-run TMPDIR=.../virgl-run \
  /system/bin/linker64 .../virgl/bin/virgl_test_server_android \
  --use-egl-surfaceless --use-gles \
  --socket-path .../virgl-run/vtest.sock
```

探针实测输出：

```
EGL 1.5
EGL_EXT_image_dma_buf_import=1
GL_RENDERER=virgl (ANGLE (Qualcomm, Vulkan 1.3.128 (Adreno (TM) 740 ...)))
gbm_bo_create/get_fd failed; using a memfd token
eglCreateImageKHR=OK glEGLImageTargetTexture2DOES=0x0
PASS
```

说明：

- `EGL_EXT_image_dma_buf_import=1` → caps 补丁生效；
- `GL_RENDERER` 含 `virgl` → virpipe → vtest → 宿主 ANGLE/Adreno 渲染；
- `eglCreateImageKHR=OK` → vtest winsys 的 NULL 钩子已补齐；
- 这次 GBM 分配在最小构建里失败，探针回退到 **memfd token**；这正好验证了
  token 方案（fd 只是身份，Mesa 按 `templ` 建纹理，不读 fd 内容）。


## 应用与构建

补丁路径相对 Mesa 源码根目录，`patch -p1`。

### Arch

```bash
git clone https://gitlab.archlinux.org/archlinux/packaging/packages/mesa.git
cd mesa
# 把补丁加入 PKGBUILD 的 source=()，并在 prepare() 里：
#   patch -Np1 -i "$srcdir/0001-virgl-vtest-dmabuf-import.patch"
makepkg -si
```

### Kali / Debian

```bash
apt-get source mesa
cd mesa-*
patch -p1 < /path/to/0001-virgl-vtest-dmabuf-import.patch
dpkg-buildpackage -b -uc -us
sudo dpkg -i ../libgl1-mesa-dri_*.deb    # 含 swrast_dri.so（virpipe）
```

重新构建后确认 `swrast_dri.so` 已更新（`dpkg -S` / `pacman -Ql mesa`）。

## 验证

`tests/egl-dmabuf-probe.c` 直接复现 KWin 的调用序列：GBM 设备 → EGL →
`eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT)`。

构建（guest 内）：

```bash
cc -O2 -o egl-dmabuf-probe egl-dmabuf-probe.c \
   $(pkg-config --cflags --libs egl glesv2 gbm)
```

运行：必须在 **virgl 会话** 里（终端会继承
`VTEST_SOCKET_NAME=/tmp/trierarch-virgl-host/vtest.sock` 和 Mesa 环境）：

```bash
LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=virpipe \
  ./egl-dmabuf-probe /dev/dri/renderD128
```

期望输出：

```
EGL 1.5
EGL_EXT_image_dma_buf_import=1
GL_RENDERER=virgl (Mesa)
eglCreateImageKHR=OK glEGLImageTargetTexture2DOES=0x0
PASS
```

补丁前：进程在 `eglCreateImageKHR` 处 SIGSEGV（NULL 函数指针）；
或扩展未公布，返回 `EGL_EXT_image_dma_buf_import is not advertised`。

## 排错

- `EGL_EXT_image_dma_buf_import=0`：`virgl_screen.c` 的修改没有进入实际加载的
  `swrast_dri.so`（检查构建/安装路径、`MESA_DEBUG=1` 输出）。
- `GL_RENDERER` 不是 `virgl`：`LIBGL_ALWAYS_SOFTWARE` / `GALLIUM_DRIVER`
  没生效，或会话没有 vtest socket。
- `eglCreateImageKHR` 返回 `EGL_BAD_PARAMETER`：检查 stride/format；
  probe 用 `GBM_FORMAT_ABGR8888` + `GBM_BO_USE_RENDERING`。

## 下一步

本补丁只让 KWin “不崩且能创建图像”。要真正显示，还需要：

1. guest `libgbm` shim：把 `gbm_bo_create` 换成宿主分配的 memfd token；
2. vtest winsys：把 `token inode ↔ res_id` 注册给宿主；
3. 宿主：AHB 分配 + `TVG_PRESENT` blit + 现有 AHB 显示路径。

见 `considerations/last-cell-feasibility-2026-10-28.md`。
