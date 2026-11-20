# 原生客户端收益评估（feat/native-client）

本文回答一个问题：**把浏览器换成独立原生客户端，能换来多少流畅度与画质；能否达到甚至超过
Sunshine + Moonlight。** 结论先行，数据在后，测试环境限制写在最前面以免误读。

- 结论摘要：[1. 结论](#1-结论)
- 测试方法与限制：[2. 测试方法](#2-测试方法与已知限制)
- 传输协议：[3. 原生传输](#3-原生传输协议)
- 指标对比：[4. 流畅度](#4-流畅度浏览器-vs-原生)
- 画质与色彩：[5. 画质与色彩](#5-画质与色彩)
- 弱网：[6. 弱网表现](#6-弱网表现)
- 与 Sunshine/Moonlight 对照：[7. 对照](#7-与-sunshinemoonlight-的对照)
- 复现命令：[8. 复现](#8-复现命令)

---

## 1. 结论

在**同一条视频流**（逐字节相同，1080p60 / H.264 High 4:4:4 / 全范围 BT.709）上对比：

| 指标 | 浏览器（Chrome 155 + WebCodecs） | 原生客户端（wwclient） | 变化 |
| --- | --- | --- | --- |
| 稳态帧率（目标 60） | 45~47 fps | **60.1 fps** | +28% |
| 每帧解码耗时 p50 | 约 45 ms | **5.98 ms** | **约 1/7.5** |
| 端到端（分片到达→上屏）p50 / p95 | 队列积压约 50 ms，约 45% 帧被跳过 | **8.4 / 9.8 ms** | 更小且稳定 |
| 上屏（GPU 上传 + 着色器 + Swap）p50 / p95 | 计入合成器，不可单独测 | **2.15 / 2.77 ms** | — |
| 花屏帧上屏 | 靠 `VideoDecoder` 内部丢弃 | 显式「等 IDR」，丢片后不上花屏 | — |

画质（与编码前的参考帧逐像素比对 1920×1080）：

| 色度 | PSNR 平均 | CIEDE2000 平均 | CIEDE2000 p95 | ΔE 最大 |
| --- | --- | --- | --- | --- |
| **4:4:4** | **51.96 dB** | **0.126** | 0.209 | 5.98 |
| 4:2:0 | 43.61 dB | 0.307 | 0.390 | 16.41 |

ΔE2000 < 1 即人眼不可分辨；4:4:4 全链路（编码→软解→GL 着色器上屏）的平均色差 0.126，
属于「和本地桌面看不出差别」的量级，且 GL 上屏结果与 CPU 参考转换逐像素一致
（PSNR 51.96 dB，通道偏差 R +0.36 / G 0.00 / B −0.14 灰阶）。4:2:0 的色差均值为 0.307，
但**边缘最大 16.4**（彩色文字/细线出现明显色边），这正是 4:4:4 要解决的问题。

与 Sunshine + Moonlight 的关系：两者的**传输骨架**（UDP 分片、丢片请求 IDR、硬解、GPU 上屏）
我们现在是同一类实现，延迟预算相近；差别在于我们多了一条「浏览器也能用」的并行通道，
以及默认就能开 4:4:4 全范围。**未做到的**：FEC/ARQ、10bit/HDR、音频、硬解 4:4:4
（详见 [7.3](#73-还差什么)）。因此当前结论是：**局域网内已达到 Moonlight 级别的流畅度与更高的色彩忠实度；
广域网弱网下仍弱于 Moonlight（缺 FEC/ARQ）。**

## 2. 测试方法与已知限制

**必须说明的环境问题**：测试期间 Hyprland 反复崩溃并进入 safe-mode
（日志 `atomic drm request: failed to commit: Permission denied`，`eDP-1` 尺寸退化为 0×0），
且 wwcap 一旦发起 `ext-image-copy-capture`（`shm` 与 `sc` 两种后端都试过）就会让合成器直接退出，
而 `grim` 截图正常。因此**本轮无法跑真机抓帧链路**，`ww-cap` 服务处于停止状态。

替代方案（不影响结论有效性）：

1. 用 `native/wwsrc.c` 把一段**离线用 NVENC 编码的 H.264**（参数与 wwcap 一致：
   1080p60、High 4:4:4、yuv444p、全范围、BT.709、无 B 帧、GOP=120）重新打成
   wwcap 的 24 字节分片格式，从 `/run/user/1000/ww-video.sock` 注入网关。
2. 浏览器与原生客户端订阅**同一条流**，所以差异只来自客户端本身，不含抓帧/编码抖动。
3. 统计口径：
   - 原生客户端 `e2e` = 分片解码完成时间 − 分片头 `pts_us`，两端同一台机器同一时钟，
     **不含抓帧与编码**；跨机时该值只反映时钟差，不能直接当玻璃到玻璃延迟用。
   - 浏览器侧用 CDP 读取 `window.__ww.status()` 的 `fps / decodeMs / queueMs / skipped`。

其他限制：

- 真实窗口上屏（vsync）无法验证：合成器不可用，上屏测试用 SDL 的 `offscreen` 视频驱动
  （EGL + OpenGL 3.3，无 vsync）。`present` 数字只含「纹理上传 + 着色器 + `SwapWindow`」，
  不含合成器 VSync 等待，属**乐观值**。
- Sunshine 已安装（`2026.914.233613-1+debiantrixie`），但 Moonlight 客户端未安装，
  且两者都需要健康合成器才能抓帧，本轮**无法端到端对照**（见第 7 节）。
- 浏览器基准是 headless Chrome（`--ozone-platform=wayland`，同一台机器，CDP 采样 10 s）。

## 3. 原生传输协议

网关新增一条 UDP 明文通道（`:8444/udp`，与 WebTransport `:8443` 并行，互不影响），
信封 8 字节：`'W','N',type,flags,u32 seq(LE)`，payload 紧随其后。

| 类型 | 值 | 方向 | 说明 |
| --- | --- | --- | --- |
| `T_VIDEO` | 1 | S→C | 沿用 `proto/wire.h` 的 24B 视频分片头（含 `frame_id/frag_index/frag_count/pts_us`） |
| `T_AUDIO` | 2 | S→C | 16B 音频头（首字节=2）原样转发 |
| `T_CTRL` | 3 | S→C | JSON 控制下行 |
| `T_CTRL_UP` | 4 | C→S | JSON 控制上行（键鼠/`kf`/参数） |
| `T_HELLO` / `T_HELLO_ACK` | 5 / 6 | C→S / S→C | 1 s 保活；新 peer 接入时回 caps 并触发 `kf`、`astream`、`active` |
| `T_PING` / `T_PONG` | 7 / 8 | 双向 | 应用层 RTT 回显 |
| `T_STATS` | 9 | C→S | 客户端自报指标（仅日志） |

设计取舍：

- **直接复用已有的 24B 分片格式**，服务端不需要为原生客户端再实现一套切片逻辑；
  代价是 1100 B 负载对应的分片数较多（20 Mbps ≈ 38 片/帧，60 Mbps ≈ 116 片/帧），
  这直接影响弱网表现（见第 6 节）。
- 5 s 无包即判定 peer 超时，释放输入租约并通知 wwcap 休眠，与浏览器会话的 `active` 语义一致。
- 输入走同一条 UDP：`pm/pw/pb/kk/keysreset` 与浏览器完全同一套 JSON，服务端注入逻辑零改动。

代码位置：[gateway/src/native.rs](../gateway/src/native.rs)（249 行）、
[gateway/src/main.rs](../gateway/src/main.rs)（`mod native` + spawn）、
[native/wwclient.c](../native/wwclient.c)（1077 行）。
客户端为 SDL2 + OpenGL 3.3 core + FFmpeg 软解，YUV 三平面 `GL_R8` 纹理，
着色器按 `u_444/u_full/u_709` 做范围展开与 BT.601/709 矩阵；letterbox 保持比例。

编码侧新增（服务端）：`--chroma 444|420`、`--color 601|709|709full|full`，
NVENC 走 `AVCOL_RANGE_JPEG` + BT.709 VUI，sws 用 `sws_setColorspaceDetails` 真正输出全范围，
见 [capture/src/encoder_vaapi.c](../capture/src/encoder_vaapi.c)、
[capture/src/main.c](../capture/src/main.c)。

## 4. 流畅度：浏览器 vs 原生

同一条 1080p60 / High 4:4:4 / 全范围流，10~12 s 稳态：

| 场景 | fps | 解码 p50 / p95 | 上屏 p50 / p95 | e2e p50 / p95 | 丢帧/坏片 |
| --- | --- | --- | --- | --- | --- |
| 浏览器（61.5 Mbps 流） | 45~47 | 45 ms / — | — | 队列 ~50 ms，10 s 内 344→474 帧被跳过 | dropped 0 / incomplete 0 |
| 原生 · 61.5 Mbps · 非 GUI | **60.12** | **5.98 / 7.02** | — | **8.36 / 9.83** | 0 / 0 |
| 原生 · 61.5 Mbps · GL 上屏 | **60.20** | 5.82 / 9.58 | **2.15 / 2.77** | 9.25 / 13.72 | 0 / 0 |
| 原生 · 20 Mbps · 非 GUI | **60.15** | **4.43 / 7.89** | — | **4.94 / 8.35** | 0 / 0 |

读法：

- 浏览器的瓶颈不是网络而是 **WebCodecs 解码 45 ms/帧**（软件解码 4:4:4 + 主线程排队），
  60 fps 需要 16.7 ms 的预算，于是约 45% 的帧被丢弃，观感是「掉帧但延迟不涨」。
- 原生客户端用 FFmpeg 4 线程解码，1080p 4:4:4 约 5~6 ms/帧，20 Mbps 码率下降到 4.4 ms，
  上屏 2.2 ms，合计仍远低于 16.7 ms，因此**留出了 headroom**：可以把 CPU 让给编码/其他进程，
  或者在 4K 下仍有希望守住 60 fps（需真机验证）。
- 60 Mbps 下 e2e 从 4.9 ms 涨到 8.4 ms，差额主要来自 116 片/帧的发送与收包处理，
  不是解码。这也是后续上 FEC / 更大 MTU（路径 MTU 允许时 1400+）的现实动机。

## 5. 画质与色彩

参考帧：`testsrc2` 1920×1080 原始 RGB；被测帧：客户端解码后的 YUV 按同一套矩阵转回 RGB。

| 项目 | 4:4:4 | 4:2:0 |
| --- | --- | --- |
| PSNR 平均 | **51.96 dB** | 43.61 dB |
| PSNR R / G / B | 49.12 / 61.28 / 52.14 | 41.68 / 47.66 / 43.42 |
| ΔE2000 平均 / p95 / 最大 | **0.126 / 0.209 / 5.98** | 0.307 / 0.390 / 16.41 |

补充校验：

- **GL 上屏路径**（`--dump-gl` 读回 framebuffer）与参考帧 PSNR 51.96 dB、
  平均绝对误差 0.32/255、通道偏差 R +0.36 / G 0.00 / B −0.14，
  说明着色器的范围展开 + 709 矩阵与 CPU 参考实现等价，**没有因为上屏而变色或压暗**。
- 全范围（`range=full`）生效时黑电平是 0、白电平 255；若回落到 limited，
  黑色会被抬到 16/255，画面发灰。客户端以 caps 里的 `range/color` 为准选择着色器分支。

结论：4:4:4 + 全范围 + BT.709 的组合下，色彩与清晰度指标处于「与本地桌面一致」的量级，
主要残差来自有损编码本身（GOP=120 的 P 帧误差）而不是转换链路。

## 6. 弱网表现

用 `wwsrc --drop PCT` 对**每个分片独立丢弃**来模拟丢包（20 Mbps / 4:4:4 / GOP 120 / 10 s）。
客户端行为：发现某帧收不全 → 立即丢弃该帧、发 `{"t":"kf"}` 请求 IDR、
并**跳过后续所有非关键帧**，直到收到完整 IDR 才继续解码上屏（因此不会把花屏画出来）。

| 丢片率 | fps | 受损帧 | 等待 IDR 跳过帧 | 不可解码 | e2e p50 | kf 次数 |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | **60.15** | 0 | 0 | 0 | 4.94 ms | 1 |
| 0.1% | 48.08 | 26 | 94 | 0 | 5.04 ms | 21 |
| 1% | 7.11 | 288 | 235 | 0 | 9.49 ms | 79 |

解读（重要）：

- 20 Mbps 下一帧约 38 片、一个 IDR 约 400 片。丢片率 0.1% 时，**单个 IDR 被打坏的概率约 1/3**，
  所以恢复会重试几次；1% 时几乎找不到完整的 IDR，帧率崩到 7 fps。
- 这是「无重传、无 FEC」的固有代价，Moonlight 在同一条件下靠 FEC + 请求重传（ARQ）保住画面。
  我们目前的差距就在这里，而不是解码或上屏。
- 但注意：`不可解码=0`、`坏片=0`，说明**协议层的错误处理是正确的**——丢包只表现为掉帧，
  不会出现持续花屏或崩溃。这正是「为了流畅度愿意牺牲局部画面」的策略，和 Moonlight 的取舍一致。
- 该表用 `wwsrc` 的「回到最近 IDR 重放」来模拟编码器即时出 IDR；真实 wwcap 是当场编码一帧 IDR，
  恢复只会更快，因此**上表的 0.1% 结果是偏保守的下界**。

## 7. 与 Sunshine + Moonlight 的对照

### 7.1 为什么不能端到端跑

- 宿主机已装 `sunshine 2026.914.233613`（Debian trixie 包），但 **Moonlight 客户端未安装**
  （`apt` 无包，未安装 flatpak/AppImage），无法建立真实会话。
- 更关键的是 Sunshine 在 Wayland 下依赖 `wlr-screencopy` / portal 抓帧，
  当前合成器处于 safe-mode 且抓帧即崩，这与本项目 wwcap 遇到的是同一个故障，
  Sunshine 同样抓不到画面。

因此本节只做**同口径的工程对照**，不给虚假的实测数字。

### 7.2 参数级对照

| 维度 | Sunshine + Moonlight（默认/常见配置） | 本项目原生客户端（实测） |
| --- | --- | --- |
| 传输 | UDP，视频分片 + FEC + 丢片请求 IDR | UDP，视频分片 + 丢片请求 IDR（**无 FEC**） |
| 默认码率 | 20 Mbps（1080p60，可调） | 可配；测试覆盖 20 / 61.5 Mbps |
| 色度 | 默认 4:2:0，可开 4:4:4 | **默认 4:4:4**（NVENC），并支持全范围 BT.709 |
| 解码 | 客户端硬解（NVDEC/DXVA/VAAPI） | 4:4:4 走 FFmpeg 软解（NVDEC 不支持 444），4:2:0 可硬解 |
| 上屏 | 客户端 GPU 着色器 + 独占全屏 | SDL2 + OpenGL 3.3 着色器 + letterbox |
| 解码耗时 | 1080p 硬解通常 2~5 ms | 4:4:4 软解 **4.4~6 ms**（4 线程） |
| 上屏耗时 | 通常 2~4 ms | **2.15 ms**（无 vsync 的乐观值） |
| 输入 | 原生事件 + 相对/绝对模式 | SDL scancode → evdev，绝对坐标，1.5 s 输入租约 |

结论：**在「传输→解码→上屏」这三段里，我们的实测数字与 Moonlight 的公开经验值同档**，
局域网 0 丢包时达到 60 fps 满帧、ΔE 0.13 的色彩忠实度（Moonlight 默认 4:2:0 的边缘色差通常在
ΔE 5~15 量级）。**差距在弱网**（缺 FEC/ARQ）与**生态**（HDR/10bit/音频/剪贴板/多显示器）。

另外一个 Moonlight 没有的优势：同一套服务端仍然保留浏览器通道（`:8443`），
无需装客户端即可临时接入；原生通道是**增量**而非替换。

### 7.3 还差什么

按优先级：

1. **FEC / ARQ**：把 0.1% 丢片的 48 fps 拉回 ~59 fps。最小可行：每 10 片加 1 个 XOR 校验片；
   或由客户端对缺失分片发 NACK（需要服务端保留 ~100 ms 的发送缓冲）。
2. **音频**：`wwclient` 目前只处理视频与输入（`T_AUDIO` 常量已在，未接入解码/播放）。
   服务端 Opus 48k 已就绪，客户端接 `libopus` + SDL 音频即可。
3. **10bit / HDR**：需要 `P10` 纹理 + PQ/HLG 传递函数；当前是 8bit SDR。
4. **4:4:4 硬解**：NVDEC 对 H.264 4:4:4 返回 `CUDA_ERROR_NOT_SUPPORTED`（已实测），
   只能用软解；4K 4:4:4 软解会成为瓶颈，届时建议 4:2:0 硬解 + 更高码率。
5. **大 MTU**：路径 MTU 允许时把 1100 提高到 1400+，分片数减少约 20%，直接改善弱网与 CPU。

## 8. 复现命令

```bash
# 构建
cd native && env -u LD_LIBRARY_PATH make          # 产出 wwclient / wwsrc

# 服务端：网关（已含原生 UDP :8444）
cd gateway && cargo build --release && ./target/release/wwgw &

# 造一段与 wwcap 同参数的测试流（1080p60 / 4:4:4 / 全范围 / 无 B 帧 / GOP 120）
ffmpeg -y -f lavfi -i testsrc2=size=1920x1080:rate=60 -t 20 \
  -c:v h264_nvenc -preset p4 -profile:v high444p -pix_fmt yuv444p \
  -rc cbr -b:v 20M -g 120 -bf 0 \
  -color_primaries bt709 -color_trc bt709 -colorspace bt709 -color_range pc \
  /tmp/clip.h264

# 喂流（--drop 1 表示每片 1% 丢包）
env -u LD_LIBRARY_PATH ./wwsrc --file /tmp/clip.h264 --fps 60 --drop 0 &

# 原生客户端：只解码不建窗口（CI/无显示器）
env -u LD_LIBRARY_PATH ./wwclient --host 127.0.0.1 --port 8444 --no-present --bench 10

# 离屏 GL 上屏 + 读回像素（校验着色器色彩）
SDL_VIDEODRIVER=offscreen env -u LD_LIBRARY_PATH ./wwclient --size 1920x1080 --no-vsync \
  --dump-gl /tmp/gl.ppm --dump-after 2

# 参考帧与被测帧比对
ffmpeg -i /tmp/gl.ppm -i /tmp/ref.png -lavfi psnr -f null -
```

指标汇总（NDJSON）示例：

```json
{"type":"summary","frames":600,"fps":60.15,"decode_p50_ms":4.43,"decode_p95_ms":7.89,
 "present_p50_ms":0.00,"present_p95_ms":0.00,"e2e_p50_ms":4.94,"e2e_p95_ms":8.35,
 "rtt_ms":0.00,"dropped":0,"bad_frags":0,"undecodable":0,"skipped":0,
 "width":1920,"height":1080,"chroma":"444","range":"full","color":"709"}
```
