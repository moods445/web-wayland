# 串流功能与性能结论（2026-11-19）

本文记录本轮排查的最终结论：功能 bug 的修复、4K60 零拷贝管线的落地过程与实测数据。供后续接手者直接据此决策。

## 1. 当前状态总览

- **4K@60fps 已达成**：独显（GTX 1050 Ti / card0）路径 `wwcap --capture sc` 实测 **59.6~60.2 fps**，转换 0.5ms/帧、抓帧延迟 ~40ms。
- 集显（Intel HD 630 / renderD129）路径保持 `--capture shm`，1080p@60fps。
- 键鼠、设置面板、分辨率/帧率/编码器切换、HUD、全屏全部功能正常。
- **三档串流策略**：平衡（QP20）/ 流畅优先（目标 60/30，QP24~30 自适应）/ 画质优先（QP16）；
  策略只改 QP 与帧率，分辨率固定。
- **4:4:4 色度**：NVENC 支持（`yuv444p` + High 4:4:4/244，码率 +30~50%，1080p 仍 55~60fps）；
  VAAPI 不支持，前端按 `/capabilities` 置灰。客户端解析 SPS 变化自动重建解码器。
- **服务常驻**：systemd 用户单元 + linger 开机自启；无会话 30 秒自动休眠（停抓帧、销毁编码器），
  新连接立即唤醒并强制 IDR。
- 自动化回归：e2e 8/8、regression 33/33 全部通过（用例见 `docs/TEST-CASES.md`）。

## 2. 已修复的功能 bug（本轮）

代码审查子智能体发现 6 个真实 bug，已逐一核实并修复：

| # | 严重程度 | 问题 | 修复位置 |
|---|---|---|---|
| 1 | 高 | WebTransport 连接失败后 `transport` 未回收，导致无法重试；`status().connected` 假阳性 | `web/src/main.js` catch 分支 + `web/src/transport.js` 新增 `connected` 标志 |
| 2 | 中 | `Ctrl+Alt+F1` 切 HUD 不写偏好，刷新后丢失 | `web/src/main.js` `onToggleHud` 回调同步 `pref.hud` 并 `savePref()` |
| 3 | 中 | 默认编码器 `vaapi` 连接时不下发，面板显示与实际编码器可能不符 | `web/src/main.js` 无条件下发 `enc` |
| 4 | 低 | 手动选分辨率后 `status().requested` 过期 | `web/src/main.js` `requestRes()` 同步 `requestedMode` |
| 5 | 低 | 丢帧降载的 `onNeedKeyframe` 回调从未接线 | `web/src/decoder.js` 构造函数接收回调 + `main.js` 接线（带 300ms 节流）|
| 6 | 低 | `?debug=1` 字节数被清零后覆盖，恒显示 0KB | `web/src/main.js` HUD loop 用 `bytesThisTick` 且去重 DEBUG 赋值 |

另外修复了一个测试断言问题：`autoMode()` 原先把浏览器窗口实际高度（如 937）记入 `requestedMode`，但服务端强制按 16:9 档位输出（1080）。现按与服务端一致的档位逻辑记录预期值（`web/src/main.js`）。

## 3. 键盘注入结论（此前遗留项，已验证关闭）

- 虚拟键盘设备 `hl-virtual-keyboard-wwcap` 已被 Hyprland 识别且为主键盘。
- 实测通过：`SUPER+2`/`SUPER+1` 工作区切换、`SUPER+Enter` 打开 foot 终端、`SUPER+Q` 关窗。
- **关键事实**：`capture/src/input.c` 当前直接发送 evdev 原始 keycode（不做 `+8` 偏移）是正确的。此前怀疑需要 `+8` 的判断是错的——Hyprland 内部会自行转换。keymap fd 需 `lseek(fd, 0, SEEK_SET)` 归零的修复仍然有效。

## 4. 4K60 性能实测数据

### 4.1 旧的 shm 路径（纯 CPU，仅作回退）

使用 `scripts/perf-test.mjs` 逐档实测（前端渲染 fps）：

| 请求分辨率 | 实际输出 | 前端 fps |
|---|---|---|
| 1920×1080 | 1920×1080 | ~30（采集节拍上限，见说明）|
| 2560×1440 | 2560×1440 | ~30 |
| 3200×1800 | 3200×1800 | ~29.3 |
| 3840×2160 | 3840×2160 | **22.0** |

4K 时服务端单帧耗时（`wwcap` 日志实测）：screencopy/shm 回读 ~46ms、CPU 色彩转换（sws_scale 4 线程 ARGB→NV12）~12ms、编码提交 ~0ms。瓶颈在 GPU→CPU 回读 + CPU 色彩转换。

> 说明：perf-test 中 1080p~1800p 显示 ~30fps 是因为该脚本请求分辨率时带了 `lock=1` 且会话刚起时的稳态采样；正常运行（自动模式）下 1080p 实测可达 58~60fps。

### 4.2 新的零拷贝路径（`--capture sc`，4K60 达成）

`wwcap --capture sc --enc nvenc --output HEADLESS-2 --max 3840x2160`，抓 4K 独显 headless 输出：

| 指标 | 实测 |
|---|---|
| 帧率 | **59.6 ~ 60.2 fps**（连续 16s 稳态）|
| 抓帧延迟（发起→ready）| ~40ms（队列里压了 ~2.4 帧，见 5.3）|
| GL 转换 | 0.5ms/帧 |
| PBO 回读拷贝 | ~1.5ms/帧 |
| 分辨率 | 3840×2160 |

动态画面验证：`蓝 → 绿 → 红 → 蓝` 每 2s 换色，解码后时间线与实际一致，无冻结、无错色。

## 5. 零拷贝管线的关键结论

### 5.1 数据流

```
compositor(NVIDIA) ──screencopy──> tiled gbm BO
                                     │ dmabuf fd
                              EGLImage（每帧重建）
                                     │ GL 采样 tiled 纹理（原生，无 detile）
                         两趟全屏 shader（Y=R8 全尺寸 / UV=RG8 半尺寸 2x2 均值）
                                     │ glReadPixels → PBO（异步）
                            fence(gpu 完成) → 主循环取回 NV12（12.4MB）
                                     │ memcpy
                              NVENC（h264_nvenc，Annex-B）
```

### 5.2 EGLImage 必须每帧销毁重建

实测：复用同一 `EGLImageKHR` 时，后续 compositor 写入**不会被看到**，画面冻结在挂接那一刻（换背景色完全无更新，且每帧新建 wl_buffer / 常驻 `cuGraphicsMapResources` 都无效）。每帧 `eglDestroyImageKHR` + `eglCreateImageKHR` 重建后一切正常，成本仅 ~0.25ms。

### 5.3 请求要压队列，回读必须异步

- compositor（Hyprland）每个刷新周期只服务**一个** screencopy。按 1:1 节拍发请求会因抖动错过刷新点，吞吐掉到 50 出头；同时压 3~4 个请求、共 5 个槽位，吞吐才稳定等于刷新率（60）。
- 同步 `glReadPixels` 每帧多花 ~12ms，3 槽位时帧率被压到 45fps；改成 **PBO + `glFenceSync`** 后恢复 60fps。
- `glClientWaitSync(fence, 0, 0)` 在 NVIDIA 上一次约 4ms（即使 timeout=0），所以既要在 `ready` 回调里取回、也要在 `tick` 里补取，保证槽位及时回收，但不能只在下一帧 ready 时才取（那样槽位占用过长，掉到 40fps）。

### 5.4 CUDA 路径已弃用

- `cuImportExternalMemory(OPAQUE_FD)` 导入 dmabuf 直接报 "unknown error"，不可用。
- `cuGraphicsGLRegisterImage` + `cuGraphicsMapResources` 在 4K tiled 纹理上要把整帧 detile，实测 18~26ms/帧（约 1.4GB/s）；常驻 map 又只能看到首帧快照。
- 因此 `capture/src/cuda_convert.cu` / `tools/embed_ptx.py` 不再参与构建。

### 5.5 色彩

BT.601 limited（与 libyuv 一致）的整数系数在 shader 里算 Y/U/V。EGL 导入的 ARGB8888 纹理按字节序实际是 **R,G,B,A**，`texelFetch` 的 `.r/.g/.b` 即 RGB。

## 6. 用法

```bash
# 独显零拷贝 4K60
scripts/switch-gpu.sh nvidia        # 内部已带 --capture sc --enc nvenc

# 集显回退（shm）
scripts/switch-gpu.sh intel         # --capture shm --enc vaapi
```

`wwcap` 直接启动：

```bash
capture/build/wwcap --video /run/user/1000/ww-video.sock \
  --ctrl /run/user/1000/wwcap-ctrl.sock \
  --capture sc --enc nvenc --output HEADLESS-2 --max 3840x2160
```

注意：`--capture sc` 会自动把 gbm 节点切到 `/dev/dri/card0`、编码器切到 `nvenc`（除非显式指定）。两条路径互斥：seatd 同一 seat 不允许两个 Hyprland 实例。

## 7. 本轮新增/改动的文件

- `capture/src/capture_sc.c`（新增，screencopy dmabuf → EGL/GL → NV12，PBO 异步回读）
- `capture/meson.build`（改成可选 egl/gl 依赖，去掉 nvcc/ptx）
- `capture/src/main.c`（`HAVE_CUDA` → `HAVE_SC_CAPTURE`）
- `scripts/switch-gpu.sh`（nvidia 分支加 `--capture sc`）
- `docs/streaming-conclusions.md`（本文）
- `web/src/main.js`、`web/src/transport.js`、`web/src/decoder.js`（6 个 bug 修复 + 档位断言）
- `scripts/regression-test.mjs`（新增，现为 33 项全功能回归）
- `scripts/perf-test.mjs`（新增，逐分辨率帧率实测）

## 8. 复现测试的命令

```bash
# e2e（8 项）
node scripts/e2e-test.mjs 9222 14000

# 全功能回归（33 项）
node scripts/regression-test.mjs 9223 14000

# 分辨率性能实测
node scripts/perf-test.mjs 9224
```

> 服务以 systemd 常驻时（`ww-session/ww-gateway/ww-cap`），日志看 `journalctl --user -u ww-cap -f`；
> 重启采集用 `scripts/restart-capture.sh`，不要直接 `pkill wwcap`（会被 `Restart=always` 拉回并产生双实例）。

测试后服务端如需恢复默认：`wwcap-ctrl.sock` 发 `{"t":"fps","v":0}` 与 `{"t":"mode","reset":1}`。

---

## 9. 两条 4K60 路线的复测（2026-11-20）

### 9.1 实测结果

| 抓帧路径 | 1080p | 1440p | 3840×2160 | 4K 单帧耗时 |
|---|---|---|---|---|
| 集显 `shm` + `vaapi`（默认）| 51~58 fps | 48~51 fps | **22 fps** | cap 13.5ms + conv 6.7ms（1080p）|
| 集显 `sc` + `vaapi`（本次修通）| **69~75 fps** | 50~52 fps | **20~22 fps** | cap 30~44ms + conv 0.3ms |
| 独显 `sc` + `nvenc` | — | — | **57.3~57.6 fps** | cap 29~35ms + conv 0.5ms |

（均为 `wwcap` 日志里的服务端 fps，无浏览器竞争；采样 ≥10s 稳态。）

### 9.2 修了什么：EGL 设备必须和抓帧卡同卡

`capture_sc.c` 原本硬编码「优先 NVIDIA」：

- `ww_sc_create()` 把 `__EGL_VENDOR_LIBRARY_FILENAMES` 锁成 `10_nvidia.json`；
- `init_egl_gl()` 遍历 `eglQueryDevicesEXT` 时取第一个 vendor 含 `NVIDIA` 的设备。

结果：在集显上抓帧时 GL 上下文建在独显上，i915 的 Y-tiled dmabuf（modifier `0x100000000000002`）
导入 dGPU 的 EGL 直接 `EGL_BAD_PARAMETER (0x300c)`，**一帧都出不来**（日志刷满 `eglCreateImage 失败`）。

修法：

- 按 DRM 节点匹配设备：`/sys/class/drm/<node>/device` 的 `realpath` 比较，`cardN` 与 `renderDNNN`
  指向同一个 PCI device 时视为同卡（避免两种节点名混淆）；
- 只有抓帧卡是 NVIDIA（`device/vendor == 0x10de`）时才强制 nvidia 厂商库，否则保留 glvnd 默认；
- 去掉 `handle_ctrl` 里「零拷贝抓帧只支持 nvenc」的门。

### 9.3 为什么修好后集显 4K 还是上不去

`conv` 从 6.7ms 掉到 **0.3ms**，证明 GPU 色彩转换与 dmabuf 导入都已生效；但 4K 仍只有 20~22fps，
`cap` 30~44ms。关键证据：**两条完全不同的实现（shm 回读 vs dmabuf+PBO）在 4K 上收敛到同一个数字**，
而 1080p/1440p 上 sc 明显更快。

→ 剩下的 30~40ms 不在本项目的回读代码里，而在 **Hyprland 在 HD 630 上合成 4K 画面、服务 screencopy
请求的节拍**（`cap` 就是「请求 → ready」）。这不是改本项目能补的，需要换合成器或换渲染路径。

### 9.4 结论

- **4K60 只有独显路径**：`scripts/switch-gpu.sh nvidia`，实测 57.3~57.6fps
  （早期记录的 59.6~60.2 与本轮差值与笔记本独显的功耗/温度状态有关）。
- **1080p 用 `sc`+`vaapi` 反而更快**（69~75fps，conv 0.3ms vs 6.7ms），
  可作为后续优化项，但需先验证长时间稳定性（本次仅跑 20s）。

复测命令：

```bash
# 集显零拷贝（必须显式指定 gbm/drm 节点，否则默认走 card0/nvenc）
./capture/build/wwcap --video /run/user/1000/ww-video.sock \
  --ctrl /tmp/wwcap-test.sock --capture sc --enc vaapi \
  --gbm /dev/dri/card1 --drm /dev/dri/renderD129 \
  --output HEADLESS-2 --max 3840x2160 --fps 60

# 独显零拷贝
./scripts/switch-gpu.sh nvidia && grep -a '\[cap\]' /tmp/wwcap.log | tail
./scripts/switch-gpu.sh intel  # 切回
```

### 9.5 集显 `sc`+`vaapi` 压测（1080p，2026-11-20）

静置 5 分钟（约 21000 帧）连续采样：

| 指标 | 结果 |
|---|---|
| 帧率 | 71.9 ~ 84.2 fps（均值 ~77.5），无衰减 |
| `cap` / `conv` | 19.9~27.2ms / 0.2~0.3ms |
| RSS | **171140 KB 全程恒定**（零增长）|
| 打开 fd | 67 全程恒定 |
| 错误日志 | 无（仅创建编码器时的 `vaCreateConfig(VPP)` 告警）|

同一桌面、同一 60Hz 指针负载下与默认路径对比（各 60s）：

| 路径 | fps | `cap` | `conv` |
|---|---|---|---|
| 集显 `sc` + `vaapi` | 75 ~ 86 | 22 ~ 28ms | 0.2 ~ 0.3ms |
| 集显 `shm` + `vaapi` | 51 ~ 54 | 12 ~ 13ms | 6.5 ~ 6.8ms |

**结论：`sc` 吞吐高约 50%、CPU 转换降到 1/20，但 `cap`（请求→ready）反而高约 10ms。**
端到端延迟未在客户端验证，因此**暂不把默认改成 `sc`**，保留为可选路径
（`WW_CAPTURE=sc WW_GBM=/dev/dri/card1 WW_ENC=vaapi`）。

另一个观察：开一个持续滚动的全屏终端窗口时，`sc` 掉到 ~45fps、`cap` 升到 ~42ms，
说明**大范围画面变化时 iGPU 的合成/回读同样会撞墙**，不只是 4K。

---

## 10. 事故复盘：鼠标"非常卡"＝双 Hyprland 嵌套（2026-11-20）

### 10.1 现象与实测

用户反馈串流里鼠标非常卡。同一时刻的服务端指标与正常值对比：

| 指标 | 异常时 | 正常 |
|---|---|---|
| 服务端 fps | **30.0** | 58.0 |
| `cap`（请求→ready）| **33.4ms** | 6.0ms |
| 输入坐标系 | 总 **3812×1080**（两个输出的并集）| 总 1920×1080 |
| `hyprctl monitors` | `WAYLAND-1 1892×1052` + `HEADLESS-2` | 只有 `HEADLESS-2`（`eDP-1` 0×0）|

### 10.2 根因

做 GPU 路径实测时，`scripts/switch-gpu.sh` 用 `pkill` 结束 Hyprland，但 `ww-session` 是
`Restart=always`，**立刻拉起了一个新的**。新实例拿不到 DRM master，退化成**嵌套会话**，
在会话里表现为一个 `WAYLAND-1` 伪输出（尺寸跟随外层窗口，1892×1052）。

后果有三层，叠加起来就是"卡"：

1. 每个画面被合成两遍（内层 1920×1080 画进外层 1892×1052 的表面）→ fps 58→30、`cap` 6→33ms；
2. 输入坐标系是**两个输出的并集**，指针位移被摊到两倍宽的区域 → 移动只有一半、且跳变；
3. 嵌套实例还没带 `--config`（README 约束 #11 的 `hyprland.lua` 回退），键位/缩放也不对。

### 10.3 处理

```bash
hyprctl output remove WAYLAND-1          # 先止血：fps 立刻回到 58、cap 13ms
systemctl --user stop ww-cap ww-gateway ww-session
pkill -f start-hyprland; pkill -x Hyprland   # 必须清干净，否则新会话继续嵌套
systemctl --user start ww-session ww-gateway ww-cap
```

恢复后：单实例带 `--config`、`eDP-1 0×0 + HEADLESS-2 1920×1080`、`布局 0,0 1920x1080（总 1920x1080）`、
58fps、`cap 6.0ms`（比事故前还快）。

### 10.4 防复发

`scripts/switch-gpu.sh` 开头新增守卫：检测到 `ww-session` active 就先停整条服务链再切换，
避免"pkill + Restart=always"制造出双实例。

### 10.5 输入追踪埋点（新增）

"卡"这类问题肉眼说不清，于是加了常开、纯内存的追踪，导出后按用户操作回放分析。

**客户端** `web/src/trace.js`（环形缓冲 60000 事件，约 10 分钟）：

- 每条出站输入补 `ts = performance.now()` 并记账（`pm/pr/pb/pw/kk`）；
- 每帧记录：到达（重组完成）、解码出帧、`drawImage` 完成（"上屏"锚点）；
- 逐条输入计算 `input → 首个上屏帧` 的延迟，维护 p50/p95/p99/max 与"帧间隔 >50ms"计数；
- 统计面板新增 `Input → screen latency` 行；设置面板「输入追踪」可导出 JSON。

**服务端** `capture/src/main.c`：

- 每条输入记录 `t_recv`（网关转到 wwcap 的时刻）与 `t_done`（注入返回），差值即注入耗时；
- 每帧记录 `id/pts/字节/发送时刻/key`；
- 每 5 秒整体覆盖写入 `/tmp/wwcap-trace.json`（一次 `fwrite`，不进热路径），
  另支持控制消息 `{"t":"trace"}` 立刻落盘。

**网关**：新增 `GET /trace`（返回服务端那份）与 `POST /trace-upload`（接收客户端自动上传）。

**自动上传（用户不需要任何操作）**：客户端连上后每 15 秒 POST 一份
「最近 2000 条事件 + 400 条延迟样本」的滚动窗口；单次输入延迟 >120ms（5 秒内只发一次）
会立刻补传，所以"卡的那一下"必定被抓住。断开连接时再发最后一份。
`fetch` 的 `keepalive` 有 64KB 上限，因此只在包体 <60KB 时启用（否则 Chrome 直接 `Failed to fetch`）。

**存储格式：NDJSON 追加**（不是覆盖写）。一行一条记录，任何时刻文件都是完整可读的行式日志：

```
{"type":"snap","takenAt":"...","meta":{...},"stats":{...},"window":{...}}   ← 一次上传的统计头
{"type":"ev","kind":"pm","t":1234.5,"x":0.3,"y":0.5}                       ← 输入/帧事件
{"type":"pair","t":1234.5,"lag":16.2}                                      ← 逐条 input→上屏 延迟
```

服务端同理：`{"type":"in","k":"pm","t":<recv_us>,"d":<注入us>,"ts":<客户端ts>,...}` 与
`{"type":"frm","id":..,"t":..,"pts":..,"n":..,"key":0|1}`。追加的好处是能 `tail -f` 实时看、
能逐行 `jq` 过滤，也不会出现"重写期间文件为空"的窗口。

**体积上限（硬性）**：`/tmp` 是 tmpfs，占用即内存，所以每个文件都封顶：

| 文件 | 上限 | 策略 |
|---|---|---|
| `ww-client-trace.jsonl` / `.jsonl.1` | 每份 2MB（`WW_TRACE_MAX_BYTES`）| 超限轮转，只保留一份旧档 |
| `wwcap-trace.jsonl` / `.jsonl.1` | 每份 2MB | 同上；每 5 秒只追加**新增**记录（`tr_in_flushed`/`tr_frm_flushed` 游标）|
| 合计 | **≈ 8MB 上限** | 跨重启保留需 `WW_TRACE_DIR=/var/tmp` |

实际速率：客户端约 95KB/15s（≈2MB/5min），服务端约 4KB/s（≈2MB/9min），所以历史大约保留 5~9 分钟。

另有上传限速：网关 1.5 秒内只接受一份（`LAST_TRACE_UPLOAD_MS`），多标签页/异常重连不会刷盘。

**分析**：`node scripts/trace-report.mjs`（不传参自动读上面两个文件；也可传手动导出的合并 JSON）

客户端与服务端按输入里的 `ts` 对齐（基线偏移取**本次会话内**的
`min(server_recv_ms - ts)`，吸收了时钟差与最小单程延迟；不能用全局 min，
否则会混入别的客户端/上一轮测试的输入导致偏移错乱），输出：

```
输入→上屏  p50 / p95 / p99 / max
收到→注入完成  p50 / p95 / max
服务端出帧间隔 p50 / p95 / max 与有效帧率
归因: 上行走廊 / 注入 / 其余(等帧+编码+下行+解码+绘制)
最差的 N 条输入（t / lag / up / inject / rest）
```

判读方式写在脚本末尾：`up` 大 → 控制流排队；`inject` 大 → wwcap 主循环被占满；
`rest` 大 → 等帧/编码/下行；`lag` 抖动大 → 掉帧或关键帧风暴。

### 10.6 追踪本身的两个坑（2026-11-20 修）

1. **窗口按条数切，指针事件会被挤出去**。`frm/dec/ren` 每帧产生 3 条，`slice(-2000)` 只覆盖
   ~12 秒；用户停手 10 秒后上传，窗口里一条 `pm` 都没有，于是"数据看起来很好"但人很卡。
   → 改成按**时长**切（30 秒）+ 报告优先选"指针样本最多的那个 snap 块（并列取新）"。
2. **"输入 → 首个上屏帧"是下界，不是反馈延迟**。它只证明"有新帧上屏"，不证明这一帧里的光标
   已经动了——恰好会给出 10ms 的漂亮数字。真正的指标见下。

### 10.7 光标可见延迟（唯一能证明"鼠标卡"的指标）

- `wwcap` 每帧记录当次注入的指针归一化坐标 `frm.cx/cy`（`cur_x/cur_y`，pm 时更新）；
- 客户端本来就有指针轨迹（`ev kind=pm`，含 `performance.now()`）；
- 报告用**位置匹配**而非对钟：取画面里光标的坐标，在客户端轨迹里找"手最后一次位于该坐标"
  的时刻，`t_render - t_hand` 就是这帧落后了多少毫秒；手停住后帧会"变正确"，这类样本自动跳过。

本机实测（headless 客户端 + localhost，软件解码）：

| 指标 | p50 | p90 | p99 | max |
|---|---|---|---|---|
| 光标可见延迟 | **38.8ms** | 48.6ms | 57.3ms | 78.6ms |

这就是"光标被编进视频"的固有代价：约 2~3 帧管线延迟。它也解释了为什么"无论分辨率、集显独显
都卡"——变的是吞吐，不变的是这条链路的**结构**。根治只能让客户端本地画光标
（Moonlight 的默认做法），代价是服务端得把合成进去的光标藏起来，否则双光标
——**已在第 11 节落地**。

实测（本机 headless，自动上传的一份快照）：输入→上屏 p50 18.9ms / p95 30.3ms，
上行走廊 1.7ms、注入 0.04ms、其余 18.0ms —— 即"等下一帧"的物理下限，管路本身没有额外延迟。

---

## 11. 本地光标：把光标从视频链路里摘出来（2026-11-20）

### 11.1 为什么必须这么做

第 10.7 节测出：光标可见延迟 p50 26ms / p90 37ms / max 52ms，而且**逐帧位移不均匀**
（受控注入 180 点、450px/s 的自测：14.8% 的帧光标完全不动，帧间隔 p95 33.8ms）。
光标编进视频，它的流畅度上限就是抓帧节奏，而抓帧在 1080p 上已经贴着 16.6ms 预算。

### 11.2 服务端隐藏光标（踩坑记录）

- `hyprctl setcursor ww-invisible 24` **在运行时无效**：用纯红主题做对照实验，
  截图里始终是默认箭头；对照组（指针不动两次截图）差异为 0，证明差异确实来自光标。
  `libXcursor` 能正常加载我们生成的文件（`XcursorFilenameLoadAllImages` 返回非空），
  所以不是主题文件的问题。
- 真正生效的方式：**启动时**给进程 `XCURSOR_THEME=ww-invisible` + `XCURSOR_SIZE=24`，
  并且把 `cursor:enable_hyprcursor` 设为 `false`（开着时 Hyprland 只认 hyprcursor 的 `.hlc`）。
  已验证：重启后指针移到两个位置各截一帧，**差异像素为 0**（画面里没有光标了）。
- 主题生成：24×24 全透明 PNG + `xcursorgen`，覆盖 44 个常见光标名
  （left_ptr/text/hand2/*-resize/...），放在 `~/.icons/ww-invisible`。

### 11.3 客户端自绘光标

- `/capabilities` 新增 `"cursor":"local"`（`WW_CURSOR=server` 可回退成旧行为）；
- 客户端 `canvas.style.cursor = 'none'`，用 SVG 箭头 `#cursor-local` 跟随 `mousemove` 的
  `clientX/clientY`，`transform: translate3d()` 定位，指针移出画布立即隐藏；
- 位置与点击映射用的是同一套归一化坐标，所以"画在哪、点在哪"严格一致；
- 代价：光标形状只剩箭头（文本 I 形、缩放箭头等全部丢失）。

### 11.4 效果

光标不再经过 抓帧→编码→网络→解码→上屏 这条链路，延迟从 26~52ms 降到**系统指针本身的延迟**（≈0），
且不再受丢帧、帧节奏抖动影响。视频侧依旧 56fps，但"拖手"感消失。

## 12. 旁挂 Sunshine + Moonlight：跑通记录（2026-11-20）

结论：**Intel 路径可用，1080p60 能跑**；NVIDIA 路径抓帧失败，不可用。全程不改项目代码，
Sunshine 只作为旁挂服务端，服务端由 `scripts/sunshine-wayland.sh` 起停。

### 12.1 运行环境

- Sunshine 2026.914.233613（Debian trixie 包），Moonlight 6.2.0（GitHub AppImage）
- 桌面：`scripts/switch-gpu.sh intel` 起的 Hyprland（i915 / `card1`），抓帧目标是 headless 输出 `HEADLESS-2`（1920x1080@60）
- 该会话下 eDP-1 仍是 `0x0`（Xorg/sddm 占 DRM master，面板 atomic commit 被拒），
  但 headless 输出不受影响，Sunshine 抓的就是它
- Sunshine 配置：`capture = wlr`、`encoder = vaapi`、`output_name = HEADLESS-2`

### 12.2 实测数据

| 项目 | 实测值 |
|---|---|
| 抓帧 | `wlr`（`zwlr_screencopy_manager_v1` v3），日志 `[wlgrab] Monitor 1 is HEADLESS-2` |
| 编码 | `h264_vaapi`（Intel iHD 25.2.3），RC framerate 60/1 |
| 码率 | 14,988,000 bps（客户端请求 20 Mbps，Sunshine 按链路折算） |
| 色彩 | SDR **Rec.601** / 8-bit / JPEG range（项目链路是 BT.709 + full range） |
| 服务端帧处理延迟 | 4 分钟稳态 avg **9.7~10.1ms**，最后 20s 窗口 min 6.9ms / max 27.2ms / avg **9.17ms** |
| 客户端 | Moonlight：`Video stream is 1920x1080x60 (format 0x1)`、`Test decode successful` |
| 连接 | `CLIENT CONNECTED` → `CLIENT DISCONNECTED` 正常，期间无 `Frame capture failed` |

延迟取自 debug 级别日志的 `Frame processing latency` 行（抓帧 + 编码，不含网络与客户端解码）。

### 12.3 部署踩坑（都已固化进脚本）

1. **NVIDIA 路径不可用**：`capture = wlr` 在独显上每帧报
   `eglCreateImageKHR ... EGL_BAD_PARAMETER` + `Frame capture failed`（Hyprland 日志里失败 ~3.6 次/秒，
   NVENC 占用 0%）。该构建没有 shm 回退，`portalgrab` 又只编了 gtk 后端、`kms` 不认 `HEADLESS` 连接器。
   另外配置值必须写 `wlr`，写 `wlgrab` 会直接 `Unable to initialize capture method`。
2. **双抓帧**：Sunshine 与 `wwcap` 同时抓 screencopy 会让合成器每帧服务两次（历史事故 58→30fps），
   脚本启动前会先停 `wwcap`。
3. **进程保活**：只加 `nohup` 时，启动它的终端/IDE 会话一结束 Sunshine 就被收走
   （表现是「刚起来就连不上」，日志停在版本号之后没有退出原因），必须 `setsid`。
4. **键鼠不生效**：Sunshine 用 `/dev/uinput`（`root:input 0660`）创建虚拟键鼠，
   `aio` 不在 `input` 组时打不开，远端口完全没反应。修法：`usermod -aG input aio`，
   并在重新登录前用 `sg input -c` 借组启动（脚本已内置分支）。
   验证：串流中注入 `SUPER+Return`，headless 桌面新开一个 foot 窗口；内核同时出现
   `libvirtualhid Keyboard/Mouse` 设备，Hyprland 识别为 `libvirtualhid-keyboard-1` / `libvirtualhid-mouse`。

### 12.4 与项目自有链路的关系

- **二选一**：两者都要抢 screencopy 抓帧，不能同时跑。
- **画质不是同一个档**：Moonlight 这条链路是 Rec.601 + JPEG range + 4:2:0，桌面文字与纯色块的色彩
  不如 `wwclient`（BT.709 + full range + 4:4:4）。
- **上限是 1080p60**：Intel 上 Hyprland 合成 4K 只有 20~22fps（见 §9），4K 只能走独显，而独显路径在
  Sunshine 下抓帧失败；本机因此没有 4K60 的 Moonlight 路线。

### 12.5 复现命令

```bash
scripts/switch-gpu.sh intel            # 起 Hyprland（若已在跑可跳过）
scripts/sunshine-wayland.sh start      # 停 wwcap → 写 3 项配置 → setsid 起 Sunshine
scripts/sunshine-wayland.sh status     # 看进程 + 最近抓帧/编码日志
scripts/sunshine-wayland.sh stop

# 客户端（同机验证用）
DISPLAY=:1 QT_QPA_PLATFORM=xcb ./Moonlight.AppImage list 127.0.0.1
DISPLAY=:1 QT_QPA_PLATFORM=xcb ./Moonlight.AppImage stream 127.0.0.1 "Desktop" \
  --1080 --fps 60 --bitrate 20000 --display-mode windowed

# 需要帧延迟数字时，临时在 sunshine.conf 加 min_log_level = debug
```
