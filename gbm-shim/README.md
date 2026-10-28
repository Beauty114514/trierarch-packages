# gbm-shim（POC）

“最后一格”的第二步：给 guest 的 KWin 一个假的 GBM 分配层。KWin 仍然用真实
DRM 节点做设备身份，但 `gbm_bo_create()` 不再分配本地 dma-buf，而是向宿主
`tvgd` 要一个 **memfd token**。

```
KWin gbm_bo_create(w,h,ABGR8888)
  → libgbm-shim（LD_PRELOAD）
    → tvgd 控制 socket
      → 宿主分配 AHardwareBuffer + memfd token
      ← token fd + stride/format
  → eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT, token fd)
    → 补丁 Mesa：按 templ 建 virgl 纹理（fd 只是身份）
```

## 构建

guest（Arch/Kali）：

```bash
make
```

产物：`libgbm-shim.so`、`tests/gbm-probe`。

## 运行

宿主先启动 token 服务（`tvgd`，见 `wayland-host/tests/tvgd.c`），socket 放在
guest 可见的 bind mount 目录，例如：

```bash
# 宿主（app uid 或 root）
tvgd /data/data/app.trierarch/files/wayland/runtime/tvgd.sock
```

guest 里：

```bash
TRIERARCH_TVG_SOCKET=/tmp/trierarch-wayland/tvgd.sock \
LD_PRELOAD=$PWD/libgbm-shim.so \
./tests/gbm-probe /dev/dri/renderD128
```

期望：

```
token ino=... stride=1024 format=0x34324241 width=256 height=256
PASS
```

宿主 `tvgd` 日志同时打印：

```
token ino=... 256x256 fmt=0x34324241 stride=1024
```

给 KWin 用时：

```bash
TRIERARCH_TVG_SOCKET=/tmp/trierarch-wayland/tvgd.sock \
LD_PRELOAD=/path/libgbm-shim.so \
kwin_wayland --wayland-display=... --xwayland ...
```

## 当前边界

- 只验证 **GBM → token**；宿主还没有把 token 对应到 virgl 资源并 blit 到 AHB，
  所以 KWin 即使跑起来也不会出画面（下一步 token 注册 + `TVG_PRESENT`）。
- `gbm_bo_map/unmap` 返回失败；KWin 的 dmabuf 路径不使用。
- 如果 `tvgd`/socket 不可用，shim 明确失败，不静默回退到真实 GBM。
