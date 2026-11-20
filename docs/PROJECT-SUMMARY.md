# web-wayland 项目完整汇总

> 本文是整个项目从需求提出到当前的完整汇总：诉求演进、系统架构、全部文件改动、全部 bug 修复、性能实测、测试工具、当前状态与待决策事项。
> 聚焦某主题时可转阅：`README.md`（上手/部署）、`docs/architecture.md`（架构/踩坑基线）、`docs/streaming-conclusions.md`（最新一轮结论与 4K60 分析）。

---

## 1. 项目目标

在浏览器里运行一个真实的 Hyprland Wayland 桌面：服务端常驻 Wayland 会话，整屏像素经硬件编码后通过 **WebTransport(UDP/QUIC)** 下发，浏览器用 **WebCodecs** 解码上屏。桌面能力全部来自 Hyprland 本体，本项目只自研串流层（抓帧、编码、传输、解码、输入注入、客户端 UI）。

用户以 Mac（4K、2x、逻辑 1080p）连接，对标 Moonlight 的 4K60 体验。

---

## 2. 用户诉求演进（完整时间线）

| 阶段 | 用户诉求 |
|---|---|
| 1 | 串流打开后鼠标很卡；如何打开应用；浏览器客户端是否是卡顿因素 |
| 2 | `google-chrome` 打不开；专用客户端（类似 Moonlight）是否比浏览器流畅 |
| 3 | 远程 Web 桌面界面左上角增加设置（分辨率、帧率、全屏）——明确是**浏览器连接侧**设置，不是 Hyprland 桌面设置 |
| 4 | Mac + Win10 + Moonlight 可 4K60，自建原生客户端抛弃浏览器能否 4K60；报告鼠标点击位置与桌面像素错位（点 Chrome 关闭无效、点邮箱却打开、地址栏无法聚焦）|
| 5 | 优先做：零拷贝抓帧 + 编码器可选独显（集显逻辑不动，客户端可切换）|
| 6 | 做，到 60fps |
| 7 | 选方案 A：桌面迁独显 + 同卡零拷贝，但要保留集显功能 |
| 8 | 严重 bug：串流界面全黑、鼠标不能用、没有基本面板 |
| 9 | 修复并验证；开子智能体全面测试并修复 bug，可用 Playwright，最好内置暴露工具/MCP 便于测试 |
| 10 | 全部执行，不要问，执行到完成，没有 bug |
| 11 | 继续（排查键盘注入）|
| 12 | 结论写入 docs；用 handoff 技能交接 |
| 13 | 把所有汇总信息写入文档（本文）|
| 14 | 三档串流策略（平衡/流畅优先/画质优先）+ 4:4:4 色度采样；策略只调 QP 与帧率，分辨率固定 |
| 15 | 修 4:4:4 切换后客户端仍按 4:2:0 解码：解析 SPS profile 变化后等 IDR 重建解码器 |
| 16 | 服务开机自启（systemd 用户单元 + linger）、无会话 30 秒自动休眠；补测试文档并提交 git |

---

## 3. 系统架构

```
Hyprland 0.55 (集显 card1 / eDP-1；或独显 headless 4K)
  │  wlr-screencopy + wl_shm（集显默认，--capture shm）
  │  wlr-screencopy + dmabuf(tiled)（独显零拷贝，--capture sc，4K60）
  │  ext-image-copy-capture + dmabuf（旧路径，全黑，已弃用）
  ▼
wwcap (C)
  ├─ 双 shm 槽位抓帧（shm 路径，抓帧与转码重叠）
  ├─ dmabuf → 每帧重建 EGLImage → GL shader 两趟出 Y/UV（sc 路径）
  ├─ PBO + fence 异步回读 12.4MB NV12（sc 路径）
  ├─ sws_scale 4 线程 ARGB→NV12（shm 路径，CPU）
  ├─ h264_vaapi（VDEnc/CQP，集显，仅 4:2:0）或 h264_nvenc（constqp/ull/p4，独显；可选 yuv444p + High 4:4:4）
  ├─ 策略控制 QP/目标帧率（平衡/流畅优先/画质优先/自定义），分辨率始终独立
  ├─ 音频：ww-desk monitor → Opus 48k/20ms（含 RED）→ ww-audio.sock
  ├─ 休眠：网关通知无会话满 30s → 停抓帧、销毁编码器
  ├─ Annex-B 分片：24B 头 + SOCK_DGRAM 发到 wwgw
  └─ 反向：zwp_virtual_keyboard / zwlr_virtual_pointer 注入键鼠
  ▼ Unix DGRAM
wwgw (Rust, wtransport)
  ├─ 8443/udp  WebTransport：视频 datagram 广播，控制双向流
  ├─ 8443/tcp  HTTPS 静态托管（仅 http/1.1 ALPN）
  ├─ IP 白名单（含 IPv4-mapped IPv6 归一）+ 证书哈希钉扎
  ├─ /healthz 健康计数；/capabilities 暴露编码器与 4:4:4 能力；新会话即发 {"t":"kf"} 强制 IDR
  ├─ 会话建立/最后一个断开发 {"t":"active","v":1|0}；空闲 30s 后 wwcap 休眠
  └─ broadcast Lagged 后请求 IDR
  ▼ WebTransport
web (JS, 无构建)
  ├─ datagram 重组（frame_id + frag_index）
  ├─ WebCodecs VideoDecoder（annexb；每帧解析 SPS，profile 变化等 IDR 后重建）
  ├─ canvas 2D 上屏（等比留黑），远端光标随视频帧下发
  ├─ 左上角设置面板（策略/分辨率/帧率/编码器/4:4:4/HUD/全屏）
  └─ window.__ww 自测接口（status/pointer/key/requestKeyframe）
```

---

## 4. 全部文件改动清单

### 4.1 抓帧与协议
| 文件 | 改动 |
|---|---|
| `capture/src/main.c` | 接入 shm 抓帧，新增 `on_shm_frame()`；CLI 参数 `--dump/--enc/--output/--capture/--max`；控制消息 `mode/fps/enc/kf`；灯光 QP 默认值；输出名改用 `w->output_name`；输入几何与初始光标设置；首片/首 AU 日志与分段耗时 |
| `capture/src/capture_shm.c`（新建）| `ww_shm_create/tick/ready/set_fps/take_cap_ms`；双 shm buffer；`fr_buffer_done()` 后才 `copy()`；本轮加了 screencopy dmabuf 一次性诊断日志 |
| `capture/src/capture_shm.h`（新建）| shm 抓帧接口与帧回调类型 |
| `capture/src/capture_sc.c`（新建）| 独显零拷贝抓帧：screencopy dmabuf(tiled) → 每帧重建 EGLImage → GL shader 出 Y/UV → PBO 异步回读 NV12；5 槽位流水线 |
| `proto/wlr-screencopy-unstable-v1.xml` | 手写协议，补 v3 的 `buffer_done` 事件（否则运行报 `has no event 6`）|
| `proto/wlr-virtual-pointer-unstable-v1.xml` | 修正 motion/button/axis 时间为**单个 uint**（原错拆 hi/lo，致参数错位）|
| `capture/meson.build` | 加入 `threads_dep`、`capture_shm.c`、生成的 screencopy 绑定；`capture_sc.c` 改为可选 `egl`/`gl` 依赖（`-DHAVE_SC_CAPTURE`），去掉 nvcc/ptx |
| `scripts/gen-protocols.sh` | 增加 screencopy 绑定生成；重新生成修正后的虚拟指针绑定 |

### 4.2 编码
| 文件 | 改动 |
|---|---|
| `capture/src/encoder_vaapi.c` | QP 生效关键：`AV_CODEC_FLAG_QSCALE` + `global_quality=qp*FF_QP2LAMBDA`；`ww_enc_create_ex(kind,drm,w,h,fps,qp)`；NVENC 后端；`ww_enc_submit_cpu()` 对 NVENC 直送 NV12；4 线程 `sws_scale` 分带转换；本轮新增 `ww_enc_create_chroma()` / `ww_enc_probe_444()`：NVENC 用 `yuv444p` + High 4:4:4(244)，VAAPI 拒绝 4:4:4；SPS profile 诊断日志 |
| `capture/src/encoder.h` | `struct ww_enc` 增 `kind` 字段；新增 chroma/探测接口声明 |

### 4.3 输入
| 文件 | 改动 |
|---|---|
| `capture/src/input.c` | `ww_input_set_geometry/set_position`；虚拟指针时间戳单个 uint32；evdev keycode 直接发送（不加 8）；keymap fd `lseek` 归零；修饰键位掩码上报；`#include <errno.h>` |

### 4.4 传输协议
| 文件 | 改动 |
|---|---|
| `proto/wire.h` | 分片头 `payload_len@12` + `pts_us@16`，packed 24 字节；删除错误的 `_reserved`（曾致 pts 错位）；新增控制消息 `strategy`（0 平衡/1 流畅/2 画质/3 自定义）与 `chroma`（1=4:4:4） |
| `web/src/proto.js` | 与 C 端一致的解析 |

### 4.5 网关
| 文件 | 改动 |
|---|---|
| `gateway/src/main.rs` | 证书路径可配 `WW_WT_CERT/WW_WT_KEY`；ACL 修 IPv4-mapped IPv6；`/healthz` + 下行计数；新会话请求 IDR；Lagged 请求 IDR；控制消息上限 64KB；`select!` 后 `down.abort()` 防泄漏；本轮新增 `/capabilities`（读 wwcap 能力文件）、`{"t":"active","v":1|0}` 会话通知（含重启后补发）、`/restart` 流程兼容 systemd |

### 4.6 前端
| 文件 | 改动 |
|---|---|
| `web/src/main.js` | 证书哈希钉扎；全屏失败不中断启动；`window.__ww` 自测接口；设置面板逻辑（`pref/requestRes/requestFps/autoMode/applyFullscreen`）；HUD fps 窗口裁剪；默认不 pointer lock（`?lock=1` 开启）；关键帧请求节流；端到端估算；修 6 个 bug + 档位断言；本轮新增三档策略 UI、4:4:4 开关与能力探测（`/capabilities` + `isConfigSupported`）、SPS profile 变化重建解码器、统计面板显示策略/色度 |
| `web/src/decoder.js` | `codecFromAnnexB()` 推导 codec；constraint 归一化 `00`、level 限 5.1；去掉 `prefer-hardware`；等首个 IDR；时间戳严格递增；error 后重建；接 `onNeedKeyframe`；本轮 `codecFromAnnexB(data, strict)` 仅在有 SPS 时返回，configure 失败按 244/100 分别回退 |
| `web/src/transport.js` | 导入 `WIRE_VERSION`；严格校验 version/fragCount/index/payloadLen；`pendingOrder` + LRU 裁剪；控制流未就绪排队；关闭清理；加 `connected` 标志 |
| `web/src/input.js` | 鼠标按 canvas 实际矩形归一化；pointer lock 后先绝对定位对齐；相对移动按 rAF 合并；**不自绘光标**（screencopy 已把 compositor 光标编进视频，再画一支会出现两个鼠标）；本轮改为绝对只发最新位置、相对累积位移 + 滚轮，失焦/断线补发按键释放，画布外不移动指针 |
| `web/index.html` | 齿轮 + 设置面板：`set-res/set-fps/set-enc/set-hud/set-fs/set-close/set-note`；本轮新增 `#set-strategy` 策略按钮组与 `#set-chroma` 4:4:4 开关（含不可用原因提示）|
| `web/styles/main.css` | HUD 去 blur 改实色；canvas 居中等比；齿轮/设置面板样式；本轮新增分段按钮 `.seg` 与色度提示样式 |

### 4.7 音频（下行）
| 文件 | 改动 |
|---|---|
| `capture/src/audio.c` / `audio.h`（新建）| PipeWire 抓 `ww-desk` monitor → S16 交织 → Opus 48k/立体声/20ms → 16B 头 + RED 冗余 → `ww-audio.sock`；`astream rx` 控制启停 |
| `web/src/audio.js` / `audio-worklet.js`（新建）| AudioDecoder(opus) → 环形缓冲（40ms 起，自适应 ≤120ms，按 seq 单调前进，小缺口用 RED 补、大缺口淡出）|
| `scripts/audio-test-tone.sh`（新建）| 服务端放 1kHz 测试音，用于验证整条音频链路 |

### 4.8 脚本与配置
| 文件 | 改动 |
|---|---|
| `scripts/switch-gpu.sh`（新建）| `intel`（card1/headless 1080p/vaapi/3200x1800）与 `nvidia`（card0/headless 4K/nvenc/3840x2160）互斥切换 |
| `scripts/run-all.sh` / `stop-all.sh` | 一键启停 Hyprland、网关、抓帧编码；本轮 Hyprland 显式 `--config`，`stop-all.sh` 检测到 systemd 时改走 `systemctl stop` |
| `scripts/install-services.sh`（新建）| 安装 `ww-*.service` 到 `~/.config/systemd/user`、`daemon-reload`、`enable-linger`、按需启动 |
| `scripts/ww-cap-start.sh`（新建）| systemd 启动器：等 Wayland socket → 复用/新建 HEADLESS 输出（1080p scale 1）→ exec wwcap |
| `scripts/restart-capture.sh`（新建）| 网页「重启采集/强制恢复桌面」：先 `forcerendererreload`，systemd 下改用 `systemctl restart`，避免双实例 |
| `scripts/cdp-shot.mjs` / `e2e-test.mjs` | CDP 截图与 8 项端到端断言工具 |
| `scripts/regression-test.mjs`（新建）| 33 项全功能回归（含策略、色度可观测、4:4:4 往返、音频、输入）|
| `scripts/perf-test.mjs`（新建）| 逐分辨率帧率实测 |
| `scripts/pipewire/50-ww-desk.conf`（新建）| 虚拟 sink `ww-desk`（只抓应用声音，避免混入麦克风）|
| `scripts/ww-session.service` / `ww-gateway.service` / `ww-cap.service` | systemd 常驻单元；`ww-cap` 用启动器 + `Restart=always`，无会话 30s 休眠 |
| `config/hypr/hyprland.conf` | `accel_profile=flat` + `force_no_accel`；`SUPER+Enter`→foot，`SUPER+D`→fuzzel |
| `README.md` / `docs/architecture.md` / `docs/streaming-conclusions.md` / `docs/TEST-CASES.md` / 本文 | 文档 |

---

## 5. 全部 bug 修复清单

### 5.1 环境与启动
| 症状 | 根因 | 修法 |
|---|---|---|
| `google-chrome` 打不开 | 缺 Qt wayland platform plugin | 安装 `qt6-wayland` |
| 桌面几乎无应用可开 | 未装终端 | 装 `foot`，`SUPER+Enter` 绑定 |
| HUD 打开后鼠标很卡 | HUD `backdrop-filter: blur(14px)` 覆盖高频 canvas | 去 blur 改实色；光标从 left/top 改 transform |

### 5.2 抓帧与画面
| 症状 | 根因 | 修法 |
|---|---|---|
| 串流全黑、无面板、鼠标不能用 | ext-image-copy-capture 的 dmabuf 抓到全黑像素 | 改用 wlr-screencopy + wl_shm |
| 首帧后无新帧 | screencopy XML 漏 `buffer_done` 事件 | 补事件并在回调里 `copy()` |
| 链接错误 `dump_ppm_once` 未定义 | 函数改名后旧调用未改 | 统一 `dump_ppm_bytes()` |
| 桌面纯黑无壁纸 | hyprpaper 0.8.4 headless 报 no target | 改 `swaybg -i ... -m fill` |
| 1080p 只有 2fps | 主循环 poll 超时 500ms | 改 4ms |

### 5.3 画质与分辨率
| 症状 | 根因 | 修法 |
|---|---|---|
| QP 参数无效、画质糊 | 只设 `global_quality` 未设 `AV_CODEC_FLAG_QSCALE` | 补标志；默认 QP20；PSNR 31→43.6dB |
| 小窗口致桌面被协商成 824×1648 / 1920×3840 | 按窗口比例推导纵横比 | 固定 16:9 档位，小窗不低于 1080p，scale 限 1~2 |
| 4K UI 极小 | 4K 仍 scale 1 | 按 DPR 设 scale（4K→2x），逻辑仍 1080p |
| 切分辨率后画面冻结 | 编码器尺寸固定，重建基准用错 | 以编码器实际尺寸为准重建 + 强制 IDR |

### 5.4 输入
| 症状 | 根因 | 修法 |
|---|---|---|
| 鼠标坐标错位（点关闭无效/点邮箱开邮箱）| 虚拟指针协议时间拆成 hi/lo 两参数致整体错位 | 改协议为单个 uint32 + 调用 |
| 鼠标线性度不对 | Hyprland 指针加速 | `accel_profile=flat` + `force_no_accel` |
| 键盘无效 | keymap fd 偏移未归零 | `lseek(fd,0,SEEK_SET)` |
| 组合键无效 | 未发 modifiers 请求 | 维护修饰键掩码并上报 |
| SUPER+2→工作区10 | 误以为需手动 +8 | 直接发 evdev 原始码（Hyprland 内部转换）|

### 5.5 传输与解码
| 症状 | 根因 | 修法 |
|---|---|---|
| 浏览器首帧黑/无画面 | 多因叠加：拿到 P 帧非 IDR；无手势全屏中断启动；头 `_reserved` 致 pts 错位；SPS codec 推导不准；强制 prefer-hardware；error 后未重建；时间戳非递增 | 逐项修复（见 4.6）+ 新会话/解码错误请求 IDR |
| 传输层偶发崩溃 | 分片校验缺失 | 严格校验 + LRU 裁剪 |
| 控制消息打爆内存 | 网关无长度限制 | 上限 64KB |
| 网关 healthz 与计数不生效 | 插入位置不对、漏加 `DATAGRAMS_OUT` | 移到 HTTP 处理路径并补计数 |

### 5.6 本轮前端 bug（审查发现并修复）
| # | 严重度 | 问题 | 修法 |
|---|---|---|---|
| 1 | 高 | 连接失败后 `transport` 未回收，无法重试；`status().connected` 假阳性 | catch 清理 + `connected` 标志 |
| 2 | 中 | HUD 快捷键不写偏好 | 回调同步 `pref.hud` 并保存 |
| 3 | 中 | 默认编码器 vaapi 不下发 | 无条件下发 `enc` |
| 4 | 低 | 手动分辨率后 `requested` 过期 | `requestRes()` 同步 `requestedMode` |
| 5 | 低 | `onNeedKeyframe` 从未接线 | Decoder 接收回调 + main 接线（300ms 节流）|
| 6 | 低 | debug 字节恒 0KB | 用 `bytesThisTick` 且去重 DEBUG 赋值 |
| 7 | 低 | e2e 分辨率断言失配（937 vs 1080）| `autoMode()` 按服务端 16:9 档位记录预期值 |

### 5.7 本轮 bug（串流策略 / 4:4:4 / 自启休眠）
| # | 问题 | 根因 | 修法 |
|---|---|---|---|
| 1 | 切 4:4:4 后服务端已是 `profile_idc=244`，前端统计仍显示 `4:2:0` | ① 诊断浏览器缓存了旧 `main.js`；② 判断依赖关键帧携带 SPS | `codecFromAnnexB(data, strict)` 仅在有 SPS 时返回；等带 SPS 的 IDR 后 `useCodec()` 重建解码器；用全新 profile 复测确认 |
| 2 | systemd 新会话里鼠标位移只有一半、`hyprctl dispatch/keyword` 语法全变 | Hyprland 0.55 默认优先加载 `hyprland.lua`，未指定 `--config` 时退回 `/usr/share/hypr` 示例配置（HEADLESS scale 2） | systemd 单元与 `run-all.sh` 统一 `start-hyprland -- --config ~/.config/hypr/hyprland.conf` |
| 3 | 手动 `pkill wwcap` 后出现两份 wwcap 抢 socket | systemd `Restart=always` 先拉起一份，`restart-capture.sh` 又手工起一份 | 脚本检测 `ww-cap` 为 active 时改走 `systemctl restart`；`stop-all.sh` 同理 |
| 4 | 休眠唤醒后首帧慢/可能花屏 | 新会话没有 IDR，且编码器是刚重建的 | 唤醒时 `need_idr=1`，网关会话建立同时下发 `kf` |

---

## 6. 性能实测

### 6.1 各阶段基线
- 1080p：约 58–60fps（早期优化后稳定）。
- 1080p + NVENC 4:4:4：仍 ~55–60fps（编码开销增加但未成瓶颈），码率约 +30~50%。
- 4K 用 shm 正确抓帧：约 15–22fps。
- 旧 dmabuf 路径曾测到 NVENC 34–36fps，但确认抓的是黑像素，**不可作为有效方案**。

### 6.2 本轮逐档实测（`scripts/perf-test.mjs`）
| 请求分辨率 | 实际输出 | 前端 fps |
|---|---|---|
| 1920×1080 | 1920×1080 | ~30（脚本稳态采样；自动模式实际 58–60）|
| 2560×1440 | 2560×1440 | ~30 |
| 3200×1800 | 3200×1800 | ~29.3 |
| 3840×2160 | 3840×2160 | **22.0** |

4K 单帧：screencopy/shm 回读 ~46ms + CPU 转换 ~12ms + 编码 ~0ms。
结论：**瓶颈在 GPU→CPU 回读与 CPU 色彩转换，不在编码器**。

### 6.3 4K60 已达成（独显零拷贝路径）

`wwcap --capture sc --enc nvenc --output HEADLESS-2 --max 3840x2160`：

| 指标 | 实测 |
|---|---|
| 帧率 | **59.6 ~ 60.2 fps**（16s 稳态）|
| GL 转换 | 0.5ms/帧 |
| PBO 回读拷贝 | ~1.5ms/帧 |
| 抓帧延迟 | ~40ms（compositor 每刷新周期只服务一个 screencopy，队列压 ~2.4 帧）|

管线：screencopy dmabuf(tiled) → 每帧重建 EGLImage → 两趟 GL shader（Y=R8、UV=RG8 半尺寸）→ PBO 异步回读 NV12 → NVENC。

关键结论（详见 `docs/streaming-conclusions.md` 第 5 节）：
1. **EGLImage 必须每帧销毁重建**，复用会冻结在挂接时刻。
2. **回读必须异步**：同步 `glReadPixels` 每帧多花 ~12ms，帧率掉到 45fps。
3. **请求要压队列**：按 1:1 节拍发请求会因抖动漏刷新点，掉到 50 出头；5 槽位才稳定 60。
4. **CUDA 互操作弃用**：`cuGraphicsMapResources` 在 4K tiled 纹理上 detile 需 18~26ms/帧；`cuImportExternalMemory` 直接报错。

旧的 shm 路径（4K 22fps）保留为集显回退。

---

## 7. 测试与验证工具

| 工具 | 用途 | 最新结果 |
|---|---|---|
| `scripts/e2e-test.mjs` | 8 项端到端断言（连接/分片/渲染/丢片/分辨率/队列/解码）| 8/8 通过 |
| `scripts/regression-test.mjs` | 33 项全功能（非黑屏/面板/策略/色度可观测/4:4:4 往返/帧率/编码器切换/HUD/输入/音频/无 JS 错误）| 33/33 通过 |
| `docs/TEST-CASES.md` | 用例台账（A~N 分类：连接、渲染、设置、输入、鼠标、稳定性、音频、串流策略、色度、自启休眠）| 与脚本一一对应 |
| `scripts/perf-test.mjs` | 逐分辨率帧率实测 | 见表 6.2 |
| `scripts/cdp-shot.mjs` | CDP 截图 | 截图确认顶栏/壁纸/面板正常 |
| `window.__ww` | 页面内自测接口 | status/pointer/key/requestKeyframe |
| `/healthz` | 网关健康计数 | ok:true，sessions/datagrams 正常 |
| `/capabilities` | 编码器与 4:4:4 能力 | `{"chroma444":0/1,"encoder":"vaapi"/"nvenc"}` |

测试后恢复默认：向 `wwcap-ctrl.sock` 发 `{"t":"fps","v":0}` 与 `{"t":"mode","reset":1}`。

---

## 8. 当前状态与已知限制

**当前状态**：集显路径 1080p@60fps、独显零拷贝 4K@60fps；三档串流策略与 4:4:4（NVENC）生效；
服务由 systemd 用户单元开机自启，无会话 30 秒自动休眠；E2E 8/8、回归 33/33 全绿。

**已知限制**：
- 4:4:4 仅 NVENC（独显）支持；Intel VAAPI 只有 4:2:0，前端会置灰并说明原因。
- 4:4:4 码率约 +30~50%，未做按场景自动选择。
- 画质优先在 4K 下受硬件限制约 20fps；流畅优先的 QP 自适应只依据实测帧率，未接丢包反馈（`stats` 已上报，服务端未消费）。
- 4K60 只在独显 `--capture sc` 路径达成；集显 shm 路径 4K 仍 ~22fps。
- NVENC 仍是从主机内存 NV12/YUV444P 上传（PBO 回读后 memcpy），尚未做显存直连。
- 音频只做下行（PipeWire monitor → Opus → AudioWorklet）；麦克风上行、剪贴板、文件传输不在本期范围。
- 音频码率固定 64kbps，不做 ABR；仅按客户端上报丢包率开关 RED 冗余（见 README 音频章节）。
- 休眠唤醒到首帧约 1 秒（重建编码器 + 强制 IDR）。
- 仓库已 `git init` 并完成首次功能提交；后续改动照常提交。

---

## 9. 方案 A：4K60 落地记录

原计划的五步已全部完成：

1. `scripts/switch-gpu.sh nvidia` 把桌面迁到独显 headless 输出（4K@60），并自动带 `--capture sc --enc nvenc`。
2. 抓帧改 screencopy dmabuf，EGL 直接导入 tiled 纹理（不走 shm 回读）。
3. 色彩转换在 GL shader 里做（BT.601 limited），替换原计划的 CUDA/NPP。
4. NVENC 编码（`h264_nvenc`，Annex-B）。
5. 集显 `--capture shm --enc vaapi` 保留为回退；两条路径互斥（seatd 同 seat 只允许一个 Hyprland）。

与原计划的偏差：CUDA 路径实测不可用（detile 18~26ms/帧），改为 GL shader；当前 NVENC 输入仍是主机内存 NV12，显存直连为后续可选优化。

---

## 10. 环境 / 运行事实

- `XDG_RUNTIME_DIR=/run/user/1000`、`WAYLAND_DISPLAY=wayland-1`、Hyprland 实例签名取 `$XDG_RUNTIME_DIR/hypr/` 最新目录。
- 抓帧命令：`wwcap --video /run/user/1000/ww-video.sock --ctrl /run/user/1000/wwcap-ctrl.sock --audio /run/user/1000/ww-audio.sock --capture shm --enc vaapi --output HEADLESS-2 --max 3200x1800`。
- 常驻方式（推荐）：`systemctl --user status ww-session ww-gateway ww-cap`，日志 `journalctl --user -u ww-cap -f`；手动调试用 `scripts/run-all.sh`，两者不要同时跑。
- 重启采集统一走 `scripts/restart-capture.sh`（systemd 下自动改用 `systemctl restart`），不要直接 `pkill wwcap`。
- 无会话 30 秒后 wwcap 自动休眠（停抓帧、销毁编码器），有连接立即唤醒并强制 IDR。
- Hyprland 启动必须带 `--config ~/.config/hypr/hyprland.conf`（0.55 默认会加载 `/usr/share/hypr/hyprland.lua`）。
- 前端 JS 改动即时生效（网关从磁盘读 `web/`），C/Rust 改动需重新 `meson compile` / `cargo build --release` 并重启对应单元。
- 物理屏 eDP-1 只广播 3840x2160（60/48Hz），不能配成 1080p（会变成 0x0 而抓不到帧）；串流一律抓 headless 输出，`hyprland.conf` 里 eDP-1 用 `preferred`。
- 控制协议：Unix DGRAM `wwcap-ctrl.sock`，一条报文一条 JSON（如 `{"t":"kk","k":125,"d":1}`）。

**约束（用户明确要求，2026-11-10，已写入项目根 `AGENTS.md`）**：禁止用 curl/Python 脚本批量抓取网页或调 API 抓数据；必须用浏览器自动化（agent-browser/playwright/chrome-use）；同时打开的网页不超过 4 个。本地 e2e/CDP 验证属于自动化测试，与此不冲突。

**交接文档**：`/tmp/handoff-web-wayland.md`（临时目录，供新 agent 接手）。
---

## 11. 输入延迟与本地光标（2026-11-20 本轮）

### 11.1 问题

用户反馈"鼠标很卡"，且**无论分辨率、集显/独显都一样**。排查发现这不是吞吐问题：
1080p 下服务端 58fps、`cap 7.5ms + conv 7.0ms`，输入注入 0.02ms，客户端解码 4ms、上屏 0.4ms。

### 11.2 测量（两轮修正后才可信）

新增常开追踪埋点：客户端 `web/src/trace.js`（输入/帧到达/解码/上屏时间戳 + 逐条
`input→上屏` 延迟），服务端 `capture/src/main.c`（每条输入 `收到→注入`、每帧
`id/pts/字节/发送时刻/光标坐标`），网关 `POST /trace-upload` 落盘 NDJSON，
`scripts/trace-report.mjs` 出报告。详见 `docs/streaming-conclusions.md` 第 10 节。

三个坑（都已修，详见 `docs/streaming-conclusions.md` 10.5 / 10.6）：

1. 事件窗口按**条数**切（2000 条 ≈ 12 秒），指针事件被挤出窗口 → 报告误判"数据不足"或给出假好数字；
2. 主指标"输入 → 首个上屏帧"只是**下界**，不证明帧里的光标动了；
3. `tr_input()` 把归一化坐标按 `(int32_t)` 存进整型，落盘全是 0。

最终指标是**光标可见延迟**：服务端每帧记录注入坐标，客户端记录指针轨迹，
用**位置匹配**（不对钟）算"手到位 → 屏幕出现该位置"的毫秒数。

### 11.3 结论数据

| 测量 | 结果 |
|---|---|
| 光标可见延迟（Mac 客户端 1080p）| p50 26ms / p90 37ms / p99 45ms / max 52ms |
| 13/64 次移动 | 2 秒内没有出现在画面上 |
| 受控自测（180 点、450px/s、60Hz 注入）| **14.8% 的帧光标完全不动**；帧间隔 p50 16.9ms / p95 33.8ms |
| 零拷贝 sc 路径对照 | 88fps 但 64.7% 帧无新位置（重复帧更多），并不更顺 |

结论：**不是延迟太大，是光标在视频里 → 流畅度被抓帧节奏锁死**，而抓帧在 1080p 已贴着 16.6ms 预算。

### 11.4 措施：本地光标（方案 B）

- 服务端：`XCURSOR_THEME=ww-invisible` + `XCURSOR_SIZE=24`（`scripts/ww-session.service`）
  且 `cursor:enable_hyprcursor = false`（`config/hypr/hyprland.conf`）→ 合成器不画光标。
  **关键事实**：`hyprctl setcursor` 对 Xcursor 主题**运行时无效**（红/透明主题对照实验证实），
  只有启动时通过环境变量才生效。验证手段：指针移到两个位置各截一帧，**差异像素 = 0**。
- 客户端：`/capabilities` 返回 `{"cursor":"local"}` 后，canvas 设 `cursor:none`，
  SVG 箭头 `#cursor-local` 用 `transform: translate3d()` 跟随真实指针，移出画布即隐藏。
- 代价：光标形状只剩箭头（文本 I 形/缩放箭头/手形丢失）。
- 回退：`WW_CURSOR=server` + 去掉 `XCURSOR_THEME`/`XCURSOR_SIZE`、`enable_hyprcursor` 改回 `true`，重启会话。

### 11.5 相关文件

`web/src/trace.js`（新增）、`scripts/trace-report.mjs`（新增）、`web/src/input.js`（`onPointer` 钩子）、
`web/src/main.js`（本地光标 + 追踪接线 + `?ds=0` 开关）、`web/index.html` / `web/styles/main.css`、
`capture/src/main.c`（追踪埋点 + `caps.cursor`）、`gateway/src/main.rs`（`/trace`、`/trace-upload`）、
`config/hypr/hyprland.conf`、`scripts/ww-session.service`、`~/.icons/ww-invisible`。

### 11.6 已知遗留

- 本地光标固定箭头、不随 DPR 缩放（4K Retina 上可能偏小，按需调整 CSS 尺寸即可）；
- 追踪落盘在 `/tmp`（tmpfs，占用即内存），上限约 8MB，历史保留 5~9 分钟；
- `?ds=0` 开关只是给 canvas `desynchronized` 做 A/B，默认仍开启。
