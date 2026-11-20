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

## 串流策略与 4:4:4 色度

设置面板「分辨率与帧率」顶部是三档策略，**只改 QP 与目标帧率，分辨率始终由下拉框决定**：

| 策略 | QP | 目标帧率 | 适用 |
|---|---|---|---|
| 平衡（默认）| 20 | 跟随下拉（默认 60）| 日常使用，清晰与带宽兼顾 |
| 流畅优先 | 24~30 按实测帧率浮动 | 60 或 30（仅这两档）| 网络波动大、优先保证跟手 |
| 画质优先 | 16 固定 | 不设目标 | 静态文档/演示；4K 下受硬件限制约 20fps |
| 自定义 | 手动拖画质滑块 | 跟随下拉 | 手动指定 QP 后自动进入 |

- 策略下发 `{"t":"strategy","v":0..3}`，服务端立即改 QP、重建编码器并强制 IDR。
- **4:4:4 色度**是独立开关：NVENC 走 `yuv444p` + High 4:4:4 Predictive（`profile_idc=244`，
  codec 串 `avc1.f4xx`），文字/彩色边缘明显更锐利，代价是码率约 +30~50%。
- 集显 VAAPI 只支持 4:2:0：服务端试开探测后写能力文件，网关 `/capabilities` 暴露给前端，
  不支持时开关置灰并显示原因；切到 NVENC 会自动恢复。
- 客户端不写死 codec：每帧解析 Annex-B 的 SPS，profile 变化时等 IDR 后重建解码器，
  统计面板会显示实际生效的色度（`4:2:0` / `4:4:4`）。

### 目标码率（CBR）：设置里设，统计里看

设置面板「码率」滑块 0~80 Mbps（`0` = 关闭，回到 QP 控制），下发 `{"t":"bitrate","v":<bps>}`：

- 服务端先查驱动的码控能力（`vaGetConfigAttributes(VAConfigAttribRateControl)`），
  **支持 CBR** 才按目标码率编码（`rc=cbr` + `bitrate/maxrate/bufsize`，NVENC 全系支持）；
  不支持就清掉目标码率并回退 CQP，能力通过 `/capabilities` 的 `rc` 字段（`cbr` / `cqp`）上报，
  前端据此置灰滑块并写明原因。HD 630 实测只有 `VA_RC_CQP`：
  `Driver does not support CBR RC mode (supported modes: CQP)`。
- 码率生效时 **QP 不再参与码控**，设置里的「画质」滑块自动置灰，避免"拖了没反应"。
- **实际码率**在状态叠层（`实际码率`）与 STATISTICS 面板（`Video bitrate`）显示，按 1 秒窗口
  统计客户端收到的字节（含音频），与设置的对照关系一眼可见。
- CQP 下实际码率随画面复杂度浮动：静态桌面 + QP 20 只有 **0.04~0.10 Mbps**（15 kbps 视频 +
  25 kbps 音频），这是 skip 帧的正常结果，不是限速。
- 开机自启可用 `WW_BITRATE=20 systemctl --user restart ww-cap` 指定（Mbps），
  手动等价：`wwcap ... --bitrate 20`。
- 高码率会放大分片量（80 Mbps ≈ 9000 分片/秒）：网关的广播队列已从 512 增到 2048
  （约 4 帧），日志也从「每 300 片打印」改成「每 5 秒打印」，否则 journald 会被刷爆。

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

## 鼠标卡顿怎么查（常开追踪，自动上传）

设置面板「输入追踪」是常开的纯内存记录，**每 15 秒自动 POST 到服务器**（单次延迟 >120ms 时立刻补一份），
用户不需要做任何操作。觉得卡就复现几下，然后在服务器上：

```bash
node scripts/trace-report.mjs          # 自动读 /tmp/ww-client-trace.json + /tmp/wwcap-trace.json
node scripts/trace-report.mjs --worst 20
node scripts/trace-report.mjs ww-trace-*.json   # 也可以用「导出追踪」得到的合并文件
```

- 客户端（`web/src/trace.js`）：每条输入带 `performance.now()`，记帧到达/解码/上屏，
  逐条算「输入 → 上屏」延迟；上传的是"最近 2000 条事件 + 400 条延迟样本"的滚动窗口，
  任意一份都够回放刚卡的那一下；
- 网关（`POST /trace-upload`）：**追加**到 `ww-client-trace.jsonl`（不覆盖，可直接 `tail -f`）；
- 服务端（`capture/src/main.c`）：每条输入记「网关收到 → 注入完成」，每帧记帧号/pts/字节/发送时刻，
  每 5 秒把**新增**记录追加到 `wwcap-trace.jsonl`。

两边都是 NDJSON，一行一条记录，可以直接看/过滤：

```bash
tail -f /tmp/ww-client-trace.jsonl | jq -c 'select(.type=="pair" and .lag>80)'
grep '"type":"in"' /tmp/wwcap-trace.jsonl | tail -5
```

| 文件 | 记录类型 | 上限 |
|---|---|---|
| `ww-client-trace.jsonl` + `.jsonl.1` | `snap`（统计头）/ `ev`（输入·帧事件）/ `pair`（逐条延迟）| 每份 2MB（`WW_TRACE_MAX_BYTES` 可调），轮转只留一份旧档 |
| `wwcap-trace.jsonl` + `.jsonl.1` | `in`（输入：收到→注入耗时）/ `frm`（出帧）| 同上 2MB |
| **合计** | | **≈ 8MB 上限**（实际追加速率：客户端 ~95KB/15s、服务端 ~4KB/s）|

`/tmp` 是 tmpfs，占用即内存；需要跨重启保留就设 `WW_TRACE_DIR=/var/tmp`。
上传侧还有 1.5 秒限速（同一网关只接受一份），多标签页/异常重连不会把盘刷爆。

判读：`up` 大 → 控制流排队；`inject` 大 → wwcap 主循环被占满；`rest` 大 → 等帧/编码/下行/解码；
`lag` 抖动大 → 掉帧或关键帧风暴。统计面板的 `Input → screen latency` 行可实时看到 p50/p95/max。

**光标可见延迟（真正的"卡不卡"指标）**：`wwcap` 每帧记录当次注入的指针归一化坐标（`frm.cx/cy`），
客户端记录自己的指针轨迹（`ev kind=pm`）。报告用**位置匹配**而不是对钟：拿画面里光标的坐标，
在客户端轨迹里找"手最后一次位于该坐标"的时刻，差值就是这帧画面里光标落后了多少毫秒。

```
=== 光标可见延迟（画面里的光标 vs 手的位置）===
n=132  p50 38.8ms  p90 48.6ms  p99 57.3ms  max 78.6ms
```

因为光标是编进视频的，这个值天然等于"两三个帧的管线延迟"；它才是用户感受到的拖手程度。
不是"输入 → 首个上屏帧"（那只证明有新帧，不证明帧里的光标动了，会显得一切正常）。

> 以上是**改造前**的基线。改成下面的本地光标后，这个指标应当降到 ≈0（光标不再经过视频链路），
> 报告里它现在用来验证"本地光标是不是真的生效了"。

## 本地光标（延迟 0ms）

上一节那个 26~50ms 是"光标在视频里"的固有代价，调参压不下去。默认部署已经改成**客户端自绘光标**：

- **服务端**：用全透明 xcursor 主题 `ww-invisible` 让合成器不把光标画进画面。
  关键约束：`hyprctl setcursor` 对 Xcursor 主题**运行时无效**（实测红/透明主题都不生效，
  只有重启时通过 `XCURSOR_THEME` 才加载得到），并且必须把 `cursor:enable_hyprcursor` 关掉
  （开着时 Hyprland 只认 `.hlc`，静默忽略）。两处都写在 `scripts/ww-session.service` 与
  `config/hypr/hyprland.conf` 里。
- **客户端**：`/capabilities` 的 `{"cursor":"local"}` 到达后才启用——canvas 设 `cursor:none`，
  用一个 SVG 箭头 `#cursor-local` 通过 `transform: translate3d()` 跟随真实指针，
  每帧只改一次 transform（GPU 合成），离开画布即隐藏。
- **代价**：光标形状只剩箭头，文本 I 形 / 缩放箭头 / 手形都没有了。
- **回退**：`WW_CURSOR=server`（服务端回报 `cursor:"server"`）+ 去掉 `XCURSOR_THEME`、
  把 `enable_hyprcursor` 改回 `true`，然后重启 `ww-session`。

主题本身是 `~/.icons/ww-invisible`（`xcursorgen` 生成，44 个常见光标名，24×24 全透明），
新机器用 `./scripts/install-cursor-theme.sh` 重建（`--check` 只检查）。

## 实测性能

帧率均为 **服务端**（`wwcap` 日志，无浏览器竞争）；每次采样 ≥10s 稳态。

| 抓帧路径 | 1920×1080 | 2560×1440 | 3840×2160 |
|---|---|---|---|
| 集显 `shm` + `vaapi`（默认）| 51~58 fps | 48~51 fps | **22 fps** |
| 集显 `sc` + `vaapi` | **69~75 fps** | 50~52 fps | **20~22 fps** |
| 独显 `sc` + `nvenc` | — | — | **57.3~57.6 fps** |

1080p 单帧：`cap 6.9ms + conv 6.6ms`（shm 路径，CPU 转换）／`cap 16.6ms + conv 0.3ms`（sc 路径）。
4K 单帧：shm `cap 46ms + conv 12ms`；sc `cap 30~44ms + conv 0.3~0.5ms`。

- **4K 上不去与编码器无关**：ffmpeg 直喂 4K60 最坏复杂度(约 106 Mbps)实测 VDEnc 编码 **54~58 fps**
  （`async_depth` 1/2/4 → 54/55/58），编码器有余量。
- **4K 的墙在抓帧节奏**：`sc` 修通后 `conv` 已降到 0.3ms，但 4K 仍是 20~22fps，`cap` 30~44ms；
  而 shm（CPU 回读）与 sc（dmabuf+PBO）两套完全不同的实现收敛到同一个数字 →
  限制来自 **Hyprland 在 HD 630 上合成 4K、服务 screencopy 的节拍**，不是本项目代码。
- 默认协商上限 `--max 3200x1800`（可用 `--max 3840x2160` 开真 4K）；
  切换：`./scripts/set-mode.sh 1920x1080@60`（编码器尺寸固定，需重启 wwcap）。

独显路径（`./scripts/switch-gpu.sh nvidia`，`--capture sc --enc nvenc`）**4K 实测 57.3~57.6fps**；
1080p 下打开 4:4:4 仍 55~60fps，码率约 +30~50%。

> `--capture sc` 的 EGL 上下文现在按 DRM 节点选择，与抓帧卡同卡。
> 早期版本硬编码「优先 NVIDIA」，在集显上会把 i915 的 Y-tiled dmabuf 导入 dGPU 的 EGL，
> 报 `EGL_BAD_PARAMETER (0x300c)` 且一帧都不出。修复后集显跑 sc 可用（1080p 更快，4K 不提升）。

## 关键约束（实测得出，改动前务必阅读）

1. **iGPU 被 Xorg 占用时无法启动 Hyprland**。DRM master 一张卡只能有一个；若 LXQt/Xorg 在跑，要么停掉它，要么按下面的方式让 Hyprland 用独显。
   当前部署：停 `sddm` + Sunshine，Hyprland 独占 `card1`(i915)。`sudo systemctl start sddm` 可随时回滚。
2. **SSH 会话没有 seat，libseat 会拒绝 DRM 访问**。必须装 `seatd` 并启用（`systemctl enable --now seatd`），并以 `LIBSEAT_BACKEND=seatd` 启动。
3. **HD 630 的 H.264 只有 `VAEntrypointEncSliceLP`（VDEnc）**，因此编码必须 `low_power=1`，且**只支持 CQP**，没有 CBR/VBR；自适应策略调节 QP 而非码率。**目标码率只有 NVENC 能用**，集显上 `/capabilities` 返回 `rc:"cqp"`，前端滑块置灰。
4. **该驱动没有 VPP**（`vainfo` 无 `VAEntrypointVideoProc`），`shm` 路径的 ARGB→NV12 只能 CPU 转换（`libswscale`）。4K 下这是主要 CPU 开销；改用 `--capture sc` 后转换在 GL shader 里做（conv 6.7ms → 0.3ms）。
5. **HEVC 只有解码入口**，H.264 是唯一硬编格式；AV1 编码在 HD630/Pascal 上都不存在。
6. **抓帧实际走 `wlr-screencopy` + `wl_shm`，不是 ext-image-copy-capture**。ext 协议的 dmabuf 在本机读出来是**全黑**（多种 modifier/同步方式都试过），而 screencopy 与 `grim` 同一路径、稳定正确。代价是每帧一次 GPU→CPU 回读（4K 约 2–4ms），可接受。
7. **抓帧必须主动按帧率发起**，且主循环 `poll` 超时不能大于帧间隔（现为 4ms）。曾因 500ms 超时把 1080p 节流到 2fps。
7. **协议头必须 24 字节**（`version,flags,stream_id,frame_id,frag_index,frag_count,payload_len` + `pts_us@16`）。曾经多一个 `_reserved` 导致 `pts_us` 错位、WebCodecs 抛 timestamp 越界。
8. **前端解码**：`avc: {format:'annexb'}`，codec 串从 SPS 推导（constraint 写 `00`，level 上限 5.1）；必须等到关键帧再解；解码器报错会被 Chrome 关闭，需重建并请求 IDR；时间戳必须严格递增。
9. **音频与视频共用 datagram 通道，靠首字节判别媒体类型**（视频头 `version=1`，音频包 `version=2`）。
   音频包头固定 16 字节，改字段必须同步 `web/src/proto.js` 的 `parseAudio`，否则音频会静默无声。
   另：Opus 编码/发送在 PipeWire RT 回调里完成，**不要**在该路径上加锁或做重活。
10. **统计面板的 Host processing latency 无法实现**：24 字节协议头里只有 `pts_us`（主机时钟），没有采集/编码时间戳，跨机时钟不可比，因此该项固定显示 `- / - / - ms`。网络延迟由控制流 `ping/pong` 测量（网关直接回环，不经过 wwcap），与 QUIC 内部 RTT 可能略有差异。
11. **Hyprland 0.55 默认优先加载 `hyprland.lua`**：启动必须显式 `--config ~/.config/hypr/hyprland.conf`，否则会退回 `/usr/share/hypr/hyprland.lua`，表现为 HEADLESS 输出 scale 2、`hyprctl keyword/dispatch` 走 Lua 语法、鼠标位移减半。
12. **systemd 常驻时不要直接 `pkill wwcap`**：`ww-cap.service` 是 `Restart=always`，会被立刻拉回；统一用 `scripts/restart-capture.sh`（重启采集）或 `scripts/stop-all.sh`（停服务）。

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
| 出现两个鼠标 / 锁定后"没有光标" | screencopy 已把 compositor 光标编进视频帧，前端再画一支就重复 | 曾经的做法是去掉前端自绘光标；**2026-11-20 起改成方案 B**：服务端用透明主题把光标从画面里去掉，再由前端自绘（见「本地光标」一节） |
| 切换分辨率后画面冻结 | 编码器尺寸固定，重建判断基准用错 | 以「编码器实际尺寸」为准重建 + 强制 IDR |
| 切 4:4:4 后前端仍显示 4:2:0 | 浏览器缓存旧 JS；且判断依赖"关键帧一定带 SPS" | SPS 严格解析 + 带 SPS 的 IDR 到达后重建解码器 |
| 新桌面里鼠标位移减半、`hyprctl` 语法全变 | Hyprland 0.55 默认优先加载 `hyprland.lua`，没指定 `--config` 就用了 `/usr/share/hypr` 示例配置（HEADLESS scale 2） | 所有启动路径显式 `--config ~/.config/hypr/hyprland.conf` |
| systemd 管理下出现两份 wwcap 抢 socket | `Restart=always` 自动拉起 + 脚本又手工起一份 | 重启/停止脚本检测到 systemd 后改走 `systemctl` |
| 串流里鼠标"非常卡"、移动只有一半 | GPU 实测时 `pkill` 掉 Hyprland，`Restart=always` 立刻拉起的新实例拿不到 DRM master → **嵌套会话**，多出一个 `WAYLAND-1` 伪输出：画面被合成两遍（fps 58→30、cap 6→33ms），输入坐标系也变成两个输出的并集 | 停整条服务链 → `pkill -x Hyprland` 清干净 → 重启；`switch-gpu.sh` 已加守卫（检测到 active 先停服务）|
| 追踪文件里一条指针事件都没有，报告说"数据不足" | 事件窗口按条数切（2000 条），而 `frm/dec/ren` 每帧 3 条 ≈ 只覆盖 12 秒；用户停手后指针事件被挤出窗口 | 改成按**时长**切（30 秒 / 上限 8000 条），报告优先选"指针样本最多的 snap 块" |
| 追踪里输入的坐标全是 0 | `tr_input()` 把归一化坐标（0~1）按 `(int32_t)` 存进整型字段 | 字段改 `double`，落盘 `%.5f` |
| `hyprctl setcursor ww-invisible` 执行成功但光标还在 | Hyprland 运行时不会重新加载 Xcursor 主题（红/透明主题对照实验证实），且 `enable_hyprcursor=1` 时只认 `.hlc` | 启动时给 `XCURSOR_THEME` + `cursor:enable_hyprcursor=false`，重启会话；验证用"指针两点各截一帧，差异像素应为 0" |

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

- 4:4:4 仅 NVENC（独显）支持；Intel VAAPI 只有 4:2:0，开关会置灰并给出原因。4:4:4 码率约 +30~50%。
- 参数自适应只覆盖 QP/目标帧率（三档策略）；丢包反馈的 QP 闭环尚未接上（`stats` 消息已上报，服务端未消费）。
- 目标码率（CBR）只在支持 CBR 的编码器上生效（NVENC）；集显 VDEnc 只有 CQP，滑块置灰，实际码率随画面浮动。
- 集显 4K 即使换成 `--capture sc`（GPU 转换 + PBO 回读）仍只有 20~22fps，`cap` 30~44ms：
  限制在 Hyprland 的 4K 合成/服务 screencopy 节拍，**4K60 目前只有独显路径**。
- 光标**不由视频下发**：服务端用全透明主题让合成器不画光标，前端自绘一个 SVG 箭头（延迟 0ms）。
  代价是形状固定为箭头，文本 I 形/缩放箭头等不再区分；`WW_CURSOR=server` 可回退成"光标在视频里"。
- 音频只做**下行**（整机混音）；浏览器麦克风上行（服务端 `ww-mic` 虚拟麦克风）与 waybar
  「远程麦克风使用中」指示属于二期，协议里已预留 `WW_IPC_AUDIO_UP` 与尾帧 `reserved` 字段。
- 音频不做码率自适应：固定 64kbps，仅按客户端上报的丢包率开关 RED 冗余（>2% 开，<0.2% 关）。
- 休眠唤醒到首帧约 1 秒（重建编码器 + 强制 IDR）。
- 剪贴板、文件传输不在本期范围。
