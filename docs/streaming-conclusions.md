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
