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
    libswscale-dev libva-dev xkbcommon libnss3-dev meson ninja-build grim \
    libpipewire-0.3-dev libopus-dev

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

`Ctrl+Alt+F1` 切换统计信息面板（默认关闭，也可在设置里勾选）。设置里的「快捷键帮助」列出键盘/鼠标操作，并逐条说明统计字段的含义。首次访问需要在 Mac 上信任一次 `certs/ca.crt`（HTTPS 页面用），WebTransport 通道用哈希钉扎，不需要额外信任。

## 开机自启与空闲休眠

服务用 **systemd 用户单元**常驻，开机即起、无需登录桌面：

```bash
./scripts/install-services.sh          # 安装单元 + enable + linger
systemctl --user status ww-session ww-gateway ww-cap
journalctl --user -u ww-cap -f         # 抓帧 / 休眠日志
```

- 单元链：`ww-session`（Hyprland）→ `ww-gateway`（:8443）→ `ww-cap`（抓帧编码）。`loginctl enable-linger` 保证无人登录也会拉起。
- **空闲休眠**：网关在最后一个客户端断开时通知 wwcap；30 秒无会话就停止抓帧、销毁编码器（CPU 接近 0、释放 VAAPI 会话），进程和桌面常驻。浏览器一连上立即唤醒并强制 IDR，约 1 秒出图。
- `ww-session` 显式带 `--config ~/.config/hypr/hyprland.conf`：新版 Hyprland 默认优先加载 `hyprland.lua`，不指定会退回 `/usr/share/hypr` 的示例配置，绑定/显示器规则全部失效。
- 回到手动调试：`systemctl --user stop ww-session ww-gateway ww-cap`，再 `./scripts/run-all.sh`。

## 分辨率协商（默认 1080p，4K 用 2x UI）

- 桌面默认 **1920×1080 @ scale 1**（`config/hypr/hyprland.conf`）。
- 客户端连上后上报 `CSS尺寸 × devicePixelRatio`，服务端据此：
  1. `hyprctl keyword monitor eDP-1,<物理W>x<物理H>@60,0x0,<scale>`（scale 上限 2）
  2. 等 screencopy 尺寸变化 → **自动销毁并重建编码器** → 强制 IDR
- Mac 4K（CSS 1920×1080、DPR 2）→ 物理 **3840×2160 @ scale 2**，逻辑仍是 1920×1080：**UI 尺寸不变、文字像素翻倍**，这就是"4K 下 2x UI"。
- 窗口尺寸变化有 300ms 防抖，再触发一次协商。

手动切换：`./scripts/set-mode.sh 1920x1080@60`（或用 `hyprctl keyword monitor` 直接改，wwcap 会自愈重建编码器）。

## 音频（下行已支持，上行麦克风在二期）

```
PipeWire 虚拟 sink ww-desk 的 monitor（只含应用声音，不含麦克风）
      │ 48kHz 立体声 · 20ms/帧 · Opus 64kbps · 每包带上一帧做 RED 冗余
      ▼
wwcap ──Unix DGRAM──▶ wwgw ──WebTransport datagram──▶ 浏览器 AudioDecoder
                                                        └─▶ AudioWorklet 环形缓冲 ─▶ 扬声器
```

- **采集目标是虚拟 sink `ww-desk`，不是硬件 sink**：本机声卡（ALC3246）默认开启 Loopback Mixing，
  硬件 sink 的 monitor 会混入麦克风/环境声，客户端听到的其实是服务器周围的声音，扬声器与内置麦
  还会形成声学回环、持续蜂鸣。`scripts/pipewire/50-ww-desk.conf` 创建 null sink 并设为默认输出，
  应用声音只进虚拟设备；wwcap 用 `target.object=ww-desk` 只抓它的 monitor。
  首次运行 `scripts/run-all.sh` 会自动安装配置并设为默认（需要重启一次 PipeWire）。
  验证：`pw-link -l | grep wwcap-audio` 应只看到 `ww-desk:monitor_FL/FR`。

- **音视频共用一个 datagram 通道**，靠首字节分流：`1` = 视频分片（24B 头），`2` = 音频包（16B 头）。
  音频包不参与视频重组，`web/src/transport.js` 的 `#onDatagram` 第一行就分流。
- **时间轴不跨机对钟**：20ms 一包使 `seq` 本身就是稳定时钟；视频帧的 `pts_us` 通过音频包到达时刻做偏移估计后再映射。
- **单包丢失可完整恢复**：每包携带 `seq-1` 的完整 Opus 帧（RED），`ahead===1` 时直接补播；
  更大缺口由 worklet 淡出遮挡（WebCodecs 不暴露 `decode_fec`，多包丢失只能遮挡）。
- **缓冲自适应**：初始 40ms，水位低于 10ms 才上调（上限 120ms），长期过高回落到 40ms。

测试整条链路：

```bash
# 服务端放一段 1kHz（浏览器里应立刻听到）
bash scripts/audio-test-tone.sh 5
# 只验服务端是否在发：应看到 pkt/s ≈ 50、kbps ≈ 64~130（开冗余时）
grep '\[aud\]' /tmp/wwcap.log
```

设置面板里的三项语义：**我的音量**只改本机 `GainNode`；**静音目标机扬声器**经 ctrl 下发
`wpctl set-mute`，并在最后一个客户端断开/进程退出时恢复原状态；**失去焦点时静音**纯本地。

## 画面卡住 / 黑屏的自救

浏览器「串流设置 → 串流控制」里有两个按钮，不需要登录服务器：

| 按钮 | 实际动作 | 代价 |
|---|---|---|
| **重启采集** | 网关跑 `scripts/restart-capture.sh`：按原参数重启 `wwcap`，先 `hyprctl dispatch forcerendererreload` 把卡住的 screencopy 队列推一下 | 画面中断 1–3s，桌面里的窗口都保留 |
| **强制恢复桌面** | 同上再加 `--deep`：连 Hyprland 会话一起重建（走 `run-all.sh`） | 桌面里的程序会被关闭，需刷新页面重连（约 10–15s） |

判定「恢复成功」不靠服务端日志：客户端先确认 `/healthz` 的 `restarts` 计数 +1（这代表网关真的接了单），
再确认帧计数继续增长，两者都满足才显示「已恢复」。5 秒内重复点击会被网关回 `busy`。

手工等价操作：

```bash
bash scripts/restart-capture.sh          # 等价于「重启采集」
bash scripts/restart-capture.sh --deep   # 等价于「强制恢复桌面」
```

> 根因提醒：Hyprland 的 `wlr-screencopy` 一旦卡住，`wwcap` 进程还活着但一直 `0 fps`，
> 而且**只杀进程不够**（拷贝请求留在合成器里），所以脚本必须先 `forcerendererreload`。

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
9. **音频与视频共用 datagram 通道，靠首字节判别媒体类型**（视频头 `version=1`，音频包 `version=2`）。
   音频包头固定 16 字节，改字段必须同步 `web/src/proto.js` 的 `parseAudio`，否则音频会静默无声。
   另：Opus 编码/发送在 PipeWire RT 回调里完成，**不要**在该路径上加锁或做重活。
10. **统计面板的 Host processing latency 无法实现**：24 字节协议头里只有 `pts_us`（主机时钟），没有采集/编码时间戳，跨机时钟不可比，因此该项固定显示 `- / - / - ms`。网络延迟由控制流 `ping/pong` 测量（网关直接回环，不经过 wwcap），与 QUIC 内部 RTT 可能略有差异。

## 已修复的坑（本轮）

| 症状 | 根因 | 修法 |
|---|---|---|
| 浏览器全黑、无面板 | ext-capture 的 dmabuf 拷贝拿到全黑像素 | 改用 screencopy + shm |
| 首帧后无新帧 | screencopy 协议漏了 `buffer_done` 事件，`copy` 发不出 | 补事件并在回调里发起 copy |
| 鼠标乱飘/顶到右边缘 | 虚拟指针协议时间是**单个 uint32**，我们按 hi/lo 两个参数发 → 参数整体错位 | 改协议定义 + 调用 |
| 鼠标线性度不对 | Hyprland 指针加速放大相对位移 | `accel_profile=flat` + `force_no_accel` |
| 鼠标完全不动（新会话连上后） | 输入权被没断干净的旧会话（异常退出的标签页/自动化测试）永久占住，新会话的 `pm` 被网关静默丢弃 | 输入权改为 1.5s 租约：owner 空闲超时后其他会话可接管 |
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
- 音频只做**下行**（整机混音）；浏览器麦克风上行（服务端 `ww-mic` 虚拟麦克风）与 waybar
  「远程麦克风使用中」指示属于二期，协议里已预留 `WW_IPC_AUDIO_UP` 与尾帧 `reserved` 字段。
- 音频不做码率自适应：固定 64kbps，仅按客户端上报的丢包率开关 RED 冗余（>2% 开，<0.2% 关）。
- 剪贴板、文件传输不在本期范围。
