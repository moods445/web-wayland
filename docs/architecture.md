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
  ├─ Annex-B 访问单元按 1100B 切片，写 24B 头后 SOCK_DGRAM 发到 wwgw（ww-video.sock）
  ├─ PipeWire 默认 sink 的 monitor → Opus 48k/立体声/20ms → 16B 头（首字节 2）+ RED 冗余
  │    → ww-audio.sock（独立 socket，避免排在视频分片后面）
  ├─ 反向：virtual-keyboard / virtual-pointer 注入键鼠
  └─ 追踪埋点：每条输入记「收到→注入」、每帧记 id/pts/字节/发送时刻/注入光标坐标
       → 每 5 秒追加 /tmp/wwcap-trace.jsonl（2MB 轮转）
  ▼ Unix DGRAM
wwgw (Rust, wtransport)
  ├─ 8443/udp  WebTransport：视频/音频都走 datagram 广播（首字节 1=视频、2=音频），控制走双向流
  ├─ 8443/tcp  HTTPS 静态托管（仅 http/1.1 ALPN）
  ├─ 192.168.0.0/24 白名单（含 IPv4-mapped IPv6 归一）
  ├─ 新会话建立即向 wwcap 发 {"t":"kf"}、{"t":"astream","rx":1} 与 {"t":"active","v":1}
  ├─ 最后一个会话断开发 {"t":"active","v":0}：wwcap 空闲 30s 后休眠（停抓帧、销毁编码器）
  ├─ /capabilities 读取 wwcap 写出的能力（色度 4:4:4 / 码控 cbr|cqp / 光标 local|server）
  ├─ /trace 返回服务端追踪；POST /trace-upload 追加客户端追踪（NDJSON，各 2MB 轮转）
  └─ astats 丢包反馈 → {"t":"ared"} 开关 RED 冗余
  ▼ WebTransport
web (JS, 无构建)
  ├─ datagram 按首字节分流：1 → 视频重组（frame_id + frag_index），2 → 音频包
  ├─ WebCodecs VideoDecoder（annexb；每帧解析 SPS，profile 变化时等 IDR 后重建解码器）
  ├─ WebCodecs AudioDecoder(opus) → AudioWorklet 环形缓冲（40ms 起，自适应 ≤120ms）
  ├─ canvas 2D 上屏（drawImage(VideoFrame)，等比留黑）
  ├─ 光标：服务端已用透明主题把 compositor 光标去掉，客户端自绘 SVG 箭头
  │    （transform 跟随系统指针，0ms；`WW_CURSOR=server` 可回退）
  └─ 键鼠捕获 → 控制流回传；追踪窗口每 15s 自动 POST 到 /trace-upload
```

> 抓帧实现有两套：默认 `--capture shm` 走 `wlr-screencopy + wl_shm`（ARGB→NV12 用 CPU）；
> `--capture dmabuf` 才是 ext-image-copy-capture + dmabuf（本机实测读出来全黑，不用于生产）；
> `--capture sc` 是 GL 零拷贝（dmabuf → EGLImage → GL shader 转 NV12 → PBO 回读），
> 4K60 只有它 + NVENC 能跑通。

## 实测数字（本机：i7-7700HQ / HD 630 / eDP-1 4K）

| 环节 | 结果 |
|---|---|
| dmabuf 抓帧 | 120/120 帧成功，零失败；像素与 `grim` 一致 |
| 抓帧驱动方式 | damage 驱动，静止不出帧；空闲 800ms 触发 `forcerendererreload` |
| VAAPI 编码 1080p60 | 3.82× 实时（≈4.4ms/帧） |
| VAAPI 编码 4K60 | 1.03× 实时（≈9.8ms/帧）→ 4K60 无余量，按档位降到 45/30fps |
| 端到端浏览器 | 3840×2160 解码上屏成功（headless Chrome + 软件 H.264） |
| 静态桌面码率 | ~0 Mbps（全黑+静止）；实际交互时按 QP 与变化面积变化 |

## 码控策略（CQP + 三档预设）

硬件只支持 CQP，因此画质控制全部落在 QP 上；**策略与分辨率解耦**，策略永不改分辨率：

| 策略 | QP | 目标帧率 |
|---|---|---|
| 平衡（默认）| 20 | 跟随分辨率档位（默认 60）|
| 流畅优先 | 24~30，按每秒实测帧率浮动 | 60 或 30（仅这两档）|
| 画质优先 | 16 固定 | 不设目标（4K 下受硬件限制约 20fps）|
| 自定义 | 手动滑块 16~30 | 跟随分辨率档位 |

- 改策略/QP 后强制一次 IDR，避免新旧质量段混在预测链里。
- 流畅优先用服务端 1 秒统计块做反馈：低于目标帧率就抬 QP（最多 30），高于目标就降回 24。
- 分辨率仍由下拉框决定；自动模式保留「先降帧率、再降分辨率」的顺序。
- **4:4:4 是独立维度**：NVENC 走 `yuv444p` + High 4:4:4 Predictive（244），VAAPI 只有 4:2:0。
  服务端试开探测写 caps 文件，网关 `/capabilities` 下发，前端据此置灰/恢复开关。
  码率约 +30~50%，适合文字/彩色边缘，不适合带宽紧张的场景。

## 为什么不需要 fork Hyprland / 写插件

`ext-image-copy-capture-v1` + `zwp_linux_dmabuf_v1` 已把 GPU 缓冲区出口开放给普通客户端，抓帧进程在 compose 之外即可拿到帧。Plan B（hyprpm 插件在渲染阶段用 EGL 导出 dmabuf）仅在这些条件成立时才需要启用：

1. 该协议不接受 dmabuf（实测接受，Plan B 未启用）；
2. 后续要做到"零 CPU 色彩转换"——需要驱动侧 VPP，而本机 iHD 不提供，只能靠插件在 GL 里转成 NV12 再导出。

## 部署形态

| 项 | 现状 |
|---|---|
| Hyprland | `LIBSEAT_BACKEND=seatd` + `AQ_DRM_DEVICES=/dev/dri/card1` + `start-hyprland -- --config ~/.config/hypr/hyprland.conf` |
| 开机自启 | `ww-session` / `ww-gateway` / `ww-cap` 三个 systemd 用户单元 + `loginctl enable-linger`，无需登录图形界面 |
| 空闲休眠 | 网关通知 `active` 会话数；无客户端 30s 后 wwcap 停抓帧、销毁编码器，新连接唤醒并强制 IDR |
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
9. Hyprland 0.55 默认优先加载 `hyprland.lua`：不带 `--config` 会退回 `/usr/share/hypr/hyprland.lua`，
   表现为 HEADLESS 输出变成 scale 2、`hyprctl keyword/dispatch` 语法全变、鼠标坐标映射减半。
   所有启动路径（systemd 单元、`run-all.sh`）都必须显式指定 `~/.config/hypr/hyprland.conf`。
10. 客户端 codec 不能写死：切 4:4:4 后 SPS profile 从 `0x64` 变 `0xf4`，必须在带 SPS 的关键帧上
    重建解码器；否则继续用旧配置喂 4:4:4 码流会直接解码报错。constraint 字节保持 `00`。
11. systemd 管理下不要直接 `pkill wwcap`：`Restart=always` 会立刻拉起，再手工起一份就会有两份
    抢同一对 socket；统一走 `scripts/restart-capture.sh`（内部检测到 systemd 会改用 `systemctl`）。
