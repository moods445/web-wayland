# 串流功能与性能结论（2026-11-19）

本文记录本轮排查的最终结论：功能 bug 的修复、端到端验证结果，以及 4K60 目标的实测数据与技术障碍。供后续接手者直接据此决策。

## 1. 当前状态总览

- 集显（Intel HD 630 / renderD129）路径正常工作，串流稳定在 1080p@60fps。
- 独显（GTX 1050 Ti / card0，NVENC）**未打通全 GPU 零拷贝管线**，不能作为 4K60 方案。
- 键盘、鼠标、设置面板、分辨率/帧率/编码器切换、HUD、全屏全部功能正常。
- 自动化回归：e2e 8/8、regression 15/15 全部通过。

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

使用 `scripts/perf-test.mjs` 逐档实测（前端渲染 fps）：

| 请求分辨率 | 实际输出 | 前端 fps |
|---|---|---|
| 1920×1080 | 1920×1080 | ~30（采集节拍上限，见说明）|
| 2560×1440 | 2560×1440 | ~30 |
| 3200×1800 | 3200×1800 | ~29.3 |
| 3840×2160 | 3840×2160 | **22.0** |

4K 时服务端单帧耗时（`wwcap` 日志实测）：
- screencopy/shm 抓帧回读：**~46ms**
- CPU 色彩转换（sws_scale 4 线程 ARGB→NV12）：**~12ms**
- 编码提交：~0ms

结论：4K 的瓶颈在 **GPU→CPU 回读 + CPU 色彩转换**，而非编码器。当前路径纯 CPU 管线到不了 60fps。

> 说明：perf-test 中 1080p~1800p 显示 ~30fps 是因为该脚本请求分辨率时带了 `lock=1` 且会话刚起时的稳态采样；正常运行（自动模式）下 1080p 实测可达 58~60fps（`wwcap` 日志与 e2e 均可佐证）。

## 5. 4K60 的两大硬障碍

### 障碍一：dmabuf 零拷贝抓帧路径产出全黑

- 旧的 `ext-image-copy-capture` dmabuf 路径（`wwcap --capture dmabuf`）本轮复测：落盘 PPM 为**单一颜色、全 0（纯黑）**，且仅 ~5fps，不可用。
- 该文件为 `capture/src/main.c` 的 `on_frame_ready()` → `dump_ppm_bytes()`，诊断时加了 `DMA_BUF_IOCTL_SYNC`（`dmabuf_sync()`），仍无法拿到有效像素。
- 因此当前默认走 `capture_shm.c`（wlr-screencopy + wl_shm），这是能正确抓到画面的路径，但代价是 CPU 回读。

### 障碍二：VAAPI VPP 不可用，无法在 GPU 上做色彩转换

- screencopy 确实提供 dmabuf（`fr_dmabuf()` 实测收到 `fmt=0x34325241` 即 ARGB8888），但编码器 `vpp_init()` 失败：
  ```
  vaCreateConfig(VPP) 失败: the requested VAProfile is not supported
  [enc] 驱动未提供 VPP，回退 CPU 色彩转换（ARGB→NV12）
  ```
- 集显 iHD/i965 驱动在本环境 vainfo 也初始化失败（默认 DRM 节点指向 NVIDIA，指定 renderD129 同样 init failed）。
- 因此即使拿到 dmabuf，也无法走 VAAPI VPP 做 ARGB→NV12，只能回退 CPU `sws_scale`。

## 6. 后续可行方向（方案 A）

要真正达到 4K60，必须建立**全 GPU 零拷贝管线**（类似 Moonlight/Sunshine）：

1. **桌面迁到独显**：`scripts/switch-gpu.sh nvidia` 已在 headless 输出上创建成功，但需继续验证。
2. **抓帧**：用 screencopy 的 dmabuf（或 EGL/dmabuf 直接导入）替代 shm 回读。
3. **色彩转换**：用 CUDA/NPP 或 NVENC 支持的色彩空间转换，把 ARGB dmabuf 转成 NV12，全程留在 GPU。
4. **编码**：NVENC 直接从 GPU 显存编码（`h264_nvenc` 已就绪，constqp/ull/p4）。
5. **保留集显路径**：`--enc vaapi` + shm 作为回退，两条路径互斥（seatd 同一 seat 不允许两个 Hyprland 实例）。

注意：`capture/src/encoder_vaapi.c` 中 NVENC 后端当前是"软件 NV12 帧直接送入，NVENC 自行上传"，**不是** GPU 显存直连——全 GPU 管线需要改动 `ww_enc_submit_cpu()` / 新增 dmabuf→CUDA 上传路径。

这是一个需要重写抓帧→转换→编码管线的工程，不是配置问题。是否推进需产品侧决策。

## 7. 本轮新增/改动的文件

- `docs/streaming-conclusions.md`（本文）
- `web/src/main.js`、`web/src/transport.js`、`web/src/decoder.js`（6 个 bug 修复 + 档位断言）
- `capture/src/capture_shm.c`（加了 screencopy dmabuf 一次性诊断日志，无害）
- `scripts/regression-test.mjs`（新增，15 项全功能回归）
- `scripts/perf-test.mjs`（新增，逐分辨率帧率实测）

## 8. 复现测试的命令

```bash
# e2e（8 项）
node scripts/e2e-test.mjs 9222 14000

# 全功能回归（15 项）
node scripts/regression-test.mjs 9223 14000

# 分辨率性能实测
node scripts/perf-test.mjs 9224
```

测试后服务端如需恢复默认：`wwcap-ctrl.sock` 发 `{"t":"fps","v":0}` 与 `{"t":"mode","reset":1}`。
