# web-wayland

浏览器里的 Hyprland 桌面：服务端常驻一个真实的 Wayland 会话，整屏像素经 GPU 编码后通过 **WebTransport(UDP/QUIC)** 下发，浏览器用 **WebCodecs** 解码上屏。桌面能力全部来自 Hyprland 本体，本项目只自研串流层。

```
Hyprland 0.55 (iGPU / eDP-1 4K)
      │ ext-image-copy-capture (dmabuf)
      ▼
wwcap   C  · dmabuf → ARGB→NV12 → h264_vaapi(VDEnc/CQP) → Annex-B 分片
      │ Unix DGRAM
      ▼
wwgw    Rust · wtransport(QUIC) + HTTPS 静态托管 + IP 白名单 + 关键帧请求
      │ WebTransport datagram / 双向流
      ▼
web     JS · WebCodecs 解码 + canvas 上屏 + 键鼠捕获（无构建步骤）
```

## 快速开始

```bash
# 1) 依赖（Debian 13，backports 已启用）
sudo apt install -y hyprland hyprland-protocols seatd waybar fuzzel hyprpaper \
    libwlroots-0.18-dev libdrm-dev libgbm-dev libavcodec-dev libavutil-dev \
    libswscale-dev libva-dev xkbcommon libnss3-dev meson ninja-build grim

# 2) 构建
./scripts/gen-protocols.sh                 # 生成 Wayland 协议绑定
meson setup capture/build && ninja -C capture/build
( cd gateway && cargo build --release )    # 需要 rustc ≥ 1.88（rustup 安装）

# 3) 证书（含 WebTransport 哈希钉扎用的 13 天自签证书）
./scripts/gen-ca.sh 192.168.0.109

# 4) 部署桌面配置 + 启动
./scripts/deploy-config.sh
./scripts/run-all.sh
```

浏览器打开 `https://<服务器IP>:8443/`：

| 参数 | 作用 |
|---|---|
| （无） | 连接页，点「进入桌面」 |
| `?auto=1` | 直接进入并请求全屏（可做成桌面快捷方式） |
| `?debug=1` | 显示左下角底层的分片/帧计数 |

`Ctrl+Alt+F1` 切换性能叠层。首次访问需要在 Mac 上信任一次 `certs/ca.crt`（HTTPS 页面用），WebTransport 通道用哈希钉扎，不需要额外信任。

## 分辨率协商（默认 1080p，4K 用 2x UI）

- 桌面默认 **1920×1080 @ scale 1**（`config/hypr/hyprland.conf`）。
- 客户端连上后上报 `CSS尺寸 × devicePixelRatio`，服务端据此：
  1. `hyprctl keyword monitor eDP-1,<物理W>x<物理H>@60,0x0,<scale>`（scale 上限 2）
  2. 等 screencopy 尺寸变化 → **自动销毁并重建编码器** → 强制 IDR
- Mac 4K（CSS 1920×1080、DPR 2）→ 物理 **3840×2160 @ scale 2**，逻辑仍是 1920×1080：**UI 尺寸不变、文字像素翻倍**，这就是"4K 下 2x UI"。
- 窗口尺寸变化有 300ms 防抖，再触发一次协商。

手动切换：`./scripts/set-mode.sh 1920x1080@60`（或用 `hyprctl keyword monitor` 直接改，wwcap 会自愈重建编码器）。

## 实测性能

| 物理输出 | 逻辑分辨率 | 帧率（服务端，无本地浏览器竞争） |
|---|---|---|
| 3840×2160 @2x | 1920×1080 | **22–27 fps**（GL 回读约 37ms/帧，硬上限） |
| 3200×1800 @1.67x | ~1920×1080 | **约 30 fps** |
| 2560×1440 @1.33x | ~1920×1080 | **约 48 fps** |
| 1920×1080 @1x | 1920×1080 | **55–58 fps** |

单帧成本拆分（4K）：抓帧回读 ~25–37ms（合成器侧）、libyuv 转换 3.5ms、NV12 上传 6.9ms、提交 0.2ms。
`screencopy` 走 wl_shm，回读是必经之路；已用双缓冲让「抓帧」与「转码」重叠。
默认协商上限 `--max 3200x1800`（可用 `--max 3840x2160` 开真 4K）。

切换：`./scripts/set-mode.sh 1920x1080@60`（编码器尺寸固定，需重启 wwcap）。

4K 上不去的根因是 **HD 630 没有 VPP**（`vainfo` 无 `VAEntrypointVideoProc`），色彩转换只能走 CPU；
加上 VDEnc 自身 4K 约 16ms，硬件上限就在 17–20fps。

## 关键约束（实测得出，改动前务必阅读）

1. **iGPU 被 Xorg 占用时无法启动 Hyprland**。DRM master 一张卡只能有一个；若 LXQt/Xorg 在跑，要么停掉它，要么按下面的方式让 Hyprland 用独显。
   当前部署：停 `sddm` + Sunshine，Hyprland 独占 `card1`(i915)。`sudo systemctl start sddm` 可随时回滚。
2. **SSH 会话没有 seat，libseat 会拒绝 DRM 访问**。必须装 `seatd` 并启用（`systemctl enable --now seatd`），并以 `LIBSEAT_BACKEND=seatd` 启动。
3. **HD 630 的 H.264 只有 `VAEntrypointEncSliceLP`（VDEnc）**，因此编码必须 `low_power=1`，且**只支持 CQP**，没有 CBR/VBR；自适应策略调节 QP 而非码率。
4. **该驱动没有 VPP**（`vainfo` 无 `VAEntrypointVideoProc`），ARGB→NV12 只能 CPU 转换（`libswscale`）。4K 下这是主要 CPU 开销。
5. **HEVC 只有解码入口**，H.264 是唯一硬编格式；AV1 编码在 HD630/Pascal 上都不存在。
6. **抓帧实际走 `wlr-screencopy` + `wl_shm`，不是 ext-image-copy-capture**。ext 协议的 dmabuf 在本机读出来是**全黑**（多种 modifier/同步方式都试过），而 screencopy 与 `grim` 同一路径、稳定正确。代价是每帧一次 GPU→CPU 回读（4K 约 2–4ms），可接受。
7. **抓帧必须主动按帧率发起**，且主循环 `poll` 超时不能大于帧间隔（现为 4ms）。曾因 500ms 超时把 1080p 节流到 2fps。
7. **协议头必须 24 字节**（`version,flags,stream_id,frame_id,frag_index,frag_count,payload_len` + `pts_us@16`）。曾经多一个 `_reserved` 导致 `pts_us` 错位、WebCodecs 抛 timestamp 越界。
8. **前端解码**：`avc: {format:'annexb'}`，codec 串从 SPS 推导（constraint 写 `00`，level 上限 5.1）；必须等到关键帧再解；解码器报错会被 Chrome 关闭，需重建并请求 IDR；时间戳必须严格递增。

## 已修复的坑（本轮）

| 症状 | 根因 | 修法 |
|---|---|---|
| 浏览器全黑、无面板 | ext-capture 的 dmabuf 拷贝拿到全黑像素 | 改用 screencopy + shm |
| 首帧后无新帧 | screencopy 协议漏了 `buffer_done` 事件，`copy` 发不出 | 补事件并在回调里发起 copy |
| 鼠标乱飘/顶到右边缘 | 虚拟指针协议时间是**单个 uint32**，我们按 hi/lo 两个参数发 → 参数整体错位 | 改协议定义 + 调用 |
| 鼠标线性度不对 | Hyprland 指针加速放大相对位移 | `accel_profile=flat` + `force_no_accel` |
| 键盘完全无效 | keymap fd 偏移未归零，compositor 读到空 keymap | `lseek(fd,0,SEEK_SET)` |
| 组合键不生效 | 没发 `modifiers` 请求 | 维护修饰键位掩码并上报 |
| 按键错位（SUPER+2→工作区10） | Hyprland 内部会 +8，不能再手动加 | 直接发 evdev 原始码 |
| 1080p 只有 2fps | 主循环 poll 超时 500ms | 改 4ms |
| 传输层偶发崩溃 | 分片校验缺失（越界索引/fragCount=0/长度不符） | 严格校验 + LRU 裁剪 |
| 控制消息可打爆内存 | 网关未限制消息长度 | 上限 64KB |
| 桌面纯黑、没有壁纸 | `hyprpaper 0.8.4` 在 headless 输出上报 `no target`，不绘制 | 改用 `swaybg -i ... -m fill` |
| 4K 下 UI 极小、几乎看不清 | 4K 仍用 scale 1，逻辑分辨率也是 4K | 协商时按 DPR 设 scale（4K → 2x），逻辑仍 1920×1080 |
| 指针锁定后光标消失 | 锁定即隐藏本地光标 | 用累积位移驱动自绘光标，始终可见 |
| 切换分辨率后画面冻结 | 编码器尺寸固定，重建判断基准用错 | 以「编码器实际尺寸」为准重建 + 强制 IDR |

## 验收

```bash
./scripts/run-all.sh
# 服务端码流合法性
ffprobe -v error -show_entries stream=codec_name,profile,level,width,height /tmp/ww-first.h264
# 抓帧正确性（与串流画面比对）
grim /tmp/check.png
# 端到端渲染（需要 Chrome）
node scripts/cdp-shot.mjs 9222 /tmp/live.png 8000
```

## 已知限制

- 光标形状未取真实位图：视频里不含光标，前端画固定箭头；`ext_image_copy_capture_cursor_session` 的位图/热点尚未接入（要"看到真实光标形状"需补这一步）。
- 自适应控制器只做了 QP 单旋钮；AIMD 与丢包反馈的闭环尚未接上（`stats` 消息已上报，服务端未消费）。
- 4K 物理极限 ~17–23fps（无 VPP，CPU 转换）；1080p 55–58fps。
- 音频、剪贴板、文件传输不在本期范围。
