# 架构与实测基线

## 数据流

```
Hyprland 0.55 (iGPU card1, eDP-1 3840x2160@60)
  │  ext-image-copy-capture-v1   客户端提交 dmabuf（linear, XRGB8888）
  ▼
wwcap (C)
  ├─ gbm 分配 dmabuf + zwp_linux_dmabuf_v1 包成 wl_buffer
  ├─ ARGB→NV12：该驱动无 VPP，走 libswscale（CPU）
  ├─ NV12 上传到 VAAPI 表面 → h264_vaapi（VDEnc / low_power / CQP）
  ├─ Annex-B 访问单元按 1100B 切片，写 24B 头后 SOCK_DGRAM 发到 wwgw
  └─ 反向：virtual-keyboard / virtual-pointer 注入键鼠
  ▼ Unix DGRAM
wwgw (Rust, wtransport)
  ├─ 8443/udp  WebTransport：视频走 datagram 广播，控制走双向流
  ├─ 8443/tcp  HTTPS 静态托管（仅 http/1.1 ALPN）
  ├─ 192.168.0.0/24 白名单（含 IPv4-mapped IPv6 归一）
  └─ 新会话建立即向 wwcap 发 {"t":"kf"} 强制 IDR
  ▼ WebTransport
web (JS, 无构建)
  ├─ datagram 重组（frame_id + frag_index）
  ├─ WebCodecs VideoDecoder（annexb，codec 由 SPS 推导）
  ├─ canvas 2D 上屏（drawImage(VideoFrame)，等比留黑）
  └─ 键鼠捕获 → 控制流回传
```

## 实测数字（本机：i7-7700HQ / HD 630 / eDP-1 4K）

| 环节 | 结果 |
|---|---|
| dmabuf 抓帧 | 120/120 帧成功，零失败；像素与 `grim` 一致 |
| 抓帧驱动方式 | damage 驱动，静止不出帧；空闲 800ms 触发 `forcerendererreload` |
| VAAPI 编码 1080p60 | 3.82× 实时（≈4.4ms/帧） |
| VAAPI 编码 4K60 | 1.03× 实时（≈9.8ms/帧）→ 4K60 无余量，按档位降到 45/30fps |
| 端到端浏览器 | 3840×2160 解码上屏成功（headless Chrome + 软件 H.264） |
| 静态桌面码率 | ~0 Mbps（全黑+静止）；实际交互时按 QP 与变化面积变化 |

## 码控策略（CQP 单旋钮）

硬件只支持 CQP，因此自适应控制器不调码率，而是调 QP：

- QP 22 = 高画质（静止页面、演示）
- QP 26 = 默认
- QP 30–34 = 网络抖动/带宽紧张时降档
- 改 QP 后强制一次 IDR，避免新旧质量段混在预测链里

降档顺序：**先降帧率（60→45→30），再降 QP，最后才降分辨率**（与"清晰优先"一致）。

## 为什么不需要 fork Hyprland / 写插件

`ext-image-copy-capture-v1` + `zwp_linux_dmabuf_v1` 已把 GPU 缓冲区出口开放给普通客户端，抓帧进程在 compose 之外即可拿到帧。Plan B（hyprpm 插件在渲染阶段用 EGL 导出 dmabuf）仅在这些条件成立时才需要启用：

1. 该协议不接受 dmabuf（实测接受，Plan B 未启用）；
2. 后续要做到"零 CPU 色彩转换"——需要驱动侧 VPP，而本机 iHD 不提供，只能靠插件在 GL 里转成 NV12 再导出。

## 部署形态

| 项 | 现状 |
|---|---|
| Hyprland | `LIBSEAT_BACKEND=seatd` + `AQ_DRM_DEVICES=/dev/dri/card1` + `start-hyprland` |
| seatd | 必需（SSH 会话无 seat，libseat 否则拒绝 DRM） |
| X11 / LXQt / Sunshine | **已停用**（iGPU 的 DRM master 唯一） |
| 回滚 | `sudo systemctl start sddm` + `systemctl --user start app-dev.lizardbyte.app.Sunshine.service` |
| 端口 | 8443 tcp+udp，避开 Sunshine 47984–47990 |

## 踩过的坑（改代码前必读）

1. `wy_vid_frag_hdr` 必须正好 24 字节；多一个 `_reserved` 会让 `pts_us` 错位到 20，前端 WebCodecs 直接抛 timestamp 越界。
2. `VideoDecoder.configure` 不能带 `hardwareAcceleration:'prefer-hardware'`——无硬件解码器的环境会直接失败。
3. 解码器一旦 error 就进入 closed 状态，之后每次 `decode` 都抛 `InvalidStateError`；必须重建并请求 IDR。
4. 配置后必须等首个关键帧再喂帧，否则 `key frame required`。
5. `avc1.` 的 constraint 字节要写 `00`，level 超过 5.1 会被判为不支持。
6. 网关 HTTPS 的 ALPN 不能给 `h2`（curl/Chrome 会等 SETTINGS 帧而报协议错误）。
7. `gbm_bo_map` 在 i915 + mesa 26 上 `unmap` 会崩（libgallium 内部），因此只做常驻映射、永不 unmap。
8. 新客户端拿到首帧前必须先有 IDR：网关在会话建立时下发 `kf`。
