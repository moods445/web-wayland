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

---

## 3. 系统架构

```
Hyprland 0.55 (集显 card1 / eDP-1；或独显 headless)
  │  wlr-screencopy + wl_shm（默认，正确抓到画面）
  │  ext-image-copy-capture + dmabuf（旧路径，全黑，已弃用）
  ▼
wwcap (C)
  ├─ 双 shm 槽位抓帧（抓帧与转码重叠）
  ├─ sws_scale 4 线程 ARGB→NV12（CPU，因驱动无 VPP）
  ├─ h264_vaapi（VDEnc/CQP，集显）或 h264_nvenc（constqp/ull/p4，独显）
  ├─ Annex-B 分片：24B 头 + SOCK_DGRAM 发到 wwgw
  └─ 反向：zwp_virtual_keyboard / zwlr_virtual_pointer 注入键鼠
  ▼ Unix DGRAM
wwgw (Rust, wtransport)
  ├─ 8443/udp  WebTransport：视频 datagram 广播，控制双向流
  ├─ 8443/tcp  HTTPS 静态托管（仅 http/1.1 ALPN）
  ├─ IP 白名单（含 IPv4-mapped IPv6 归一）+ 证书哈希钉扎
  ├─ /healthz 健康计数；新会话即发 {"t":"kf"} 强制 IDR
  └─ broadcast Lagged 后请求 IDR
  ▼ WebTransport
web (JS, 无构建)
  ├─ datagram 重组（frame_id + frag_index）
  ├─ WebCodecs VideoDecoder（annexb，codec 由 SPS 推导）
  ├─ canvas 2D 上屏（等比留黑）+ 自绘光标
  ├─ 左上角设置面板（分辨率/帧率/编码器/HUD/全屏）
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
| `proto/wlr-screencopy-unstable-v1.xml` | 手写协议，补 v3 的 `buffer_done` 事件（否则运行报 `has no event 6`）|
| `proto/wlr-virtual-pointer-unstable-v1.xml` | 修正 motion/button/axis 时间为**单个 uint**（原错拆 hi/lo，致参数错位）|
| `capture/meson.build` | 加入 `threads_dep`、`capture_shm.c`、生成的 screencopy 绑定 |
| `scripts/gen-protocols.sh` | 增加 screencopy 绑定生成；重新生成修正后的虚拟指针绑定 |

### 4.2 编码
| 文件 | 改动 |
|---|---|
| `capture/src/encoder_vaapi.c` | QP 生效关键：`AV_CODEC_FLAG_QSCALE` + `global_quality=qp*FF_QP2LAMBDA`；`ww_enc_create_ex(kind,drm,w,h,fps,qp)`；NVENC 后端；`ww_enc_submit_cpu()` 对 NVENC 直送 NV12；4 线程 `sws_scale` 分带转换 |
| `capture/src/encoder.h` | `struct ww_enc` 增 `kind` 字段 |

### 4.3 输入
| 文件 | 改动 |
|---|---|
| `capture/src/input.c` | `ww_input_set_geometry/set_position`；虚拟指针时间戳单个 uint32；evdev keycode 直接发送（不加 8）；keymap fd `lseek` 归零；修饰键位掩码上报；`#include <errno.h>` |

### 4.4 传输协议
| 文件 | 改动 |
|---|---|
| `proto/wire.h` | 分片头 `payload_len@12` + `pts_us@16`，packed 24 字节；删除错误的 `_reserved`（曾致 pts 错位）|
| `web/src/proto.js` | 与 C 端一致的解析 |

### 4.5 网关
| 文件 | 改动 |
|---|---|
| `gateway/src/main.rs` | 证书路径可配 `WW_WT_CERT/WW_WT_KEY`；ACL 修 IPv4-mapped IPv6；`/healthz` + 下行计数；新会话请求 IDR；Lagged 请求 IDR；控制消息上限 64KB；`select!` 后 `down.abort()` 防泄漏 |

### 4.6 前端
| 文件 | 改动 |
|---|---|
| `web/src/main.js` | 证书哈希钉扎；全屏失败不中断启动；`window.__ww` 自测接口；设置面板逻辑（`pref/requestRes/requestFps/autoMode/applyFullscreen`）；HUD fps 窗口裁剪；默认不 pointer lock（`?lock=1` 开启）；关键帧请求节流；端到端估算；本轮修 6 个 bug + 档位断言 |
| `web/src/decoder.js` | `codecFromAnnexB()` 推导 codec；constraint 归一化 `00`、level 限 5.1；去掉 `prefer-hardware`；等首个 IDR；时间戳严格递增；error 后重建；本轮接 `onNeedKeyframe` |
| `web/src/transport.js` | 导入 `WIRE_VERSION`；严格校验 version/fragCount/index/payloadLen；`pendingOrder` + LRU 裁剪；控制流未就绪排队；关闭清理；本轮加 `connected` 标志 |
| `web/src/input.js` | 鼠标按 canvas 实际矩形归一化；自绘光标用 `translate3d`；pointer lock 后先绝对定位对齐；相对移动按 rAF 合并 |
| `web/index.html` | 左上角齿轮 + 设置面板：`set-res/set-fps/set-enc/set-hud/set-fs/set-close/set-note` |
| `web/styles/main.css` | HUD 去 blur 改实色；光标 `transform` + `will-change`；canvas 居中等比；齿轮/设置面板样式 |

### 4.7 脚本与配置
| 文件 | 改动 |
|---|---|
| `scripts/switch-gpu.sh`（新建）| `intel`（card1/eDP-1/vaapi/3200x1800）与 `nvidia`（card0/headless/nvenc/3840x2160）互斥切换 |
| `scripts/run-all.sh` / `stop-all.sh` | 一键启停 Hyprland、网关、抓帧编码 |
| `scripts/cdp-shot.mjs` / `e2e-test.mjs` | CDP 截图与端到端断言工具 |
| `scripts/regression-test.mjs`（新建）| 15 项全功能回归 |
| `scripts/perf-test.mjs`（新建）| 逐分辨率帧率实测 |
| `scripts/ww-session.service` / `ww-gateway.service` / `ww-cap.service` | systemd 常驻单元 |
| `config/hypr/hyprland.conf` | `accel_profile=flat` + `force_no_accel`；`SUPER+Enter`→foot，`SUPER+D`→fuzzel |
| `README.md` / `docs/architecture.md` / `docs/streaming-conclusions.md` / 本文 | 文档 |

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

---

## 6. 性能实测

### 6.1 各阶段基线
- 1080p：约 58–60fps（早期优化后稳定）。
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

### 6.3 4K60 两大硬障碍
1. **dmabuf 抓帧全黑**：`--capture dmabuf` 落盘 PPM 单色全 0，加 `DMA_BUF_IOCTL_SYNC` 也无效。
2. **VAAPI VPP 不可用**：`vaCreateConfig(VPP)` 失败；screencopy 虽提供 dmabuf（实测 `fmt=0x34325241` ARGB8888），但无法在 GPU 上做 ARGB→NV12，只能 CPU 回退。

---

## 7. 测试与验证工具

| 工具 | 用途 | 最新结果 |
|---|---|---|
| `scripts/e2e-test.mjs` | 8 项端到端断言（连接/分片/渲染/丢片/分辨率/队列/解码）| 8/8 通过 |
| `scripts/regression-test.mjs` | 15 项全功能（非黑屏/面板/帧率/编码器切换/HUD/输入/无 JS 错误）| 15/15 通过 |
| `scripts/perf-test.mjs` | 逐分辨率帧率实测 | 见表 6.2 |
| `scripts/cdp-shot.mjs` | CDP 截图 | 截图确认顶栏/壁纸/面板正常 |
| `window.__ww` | 页面内自测接口 | status/pointer/key/requestKeyframe |
| `/healthz` | 网关健康计数 | ok:true，sessions/datagrams 正常 |

测试后恢复默认：向 `wwcap-ctrl.sock` 发 `{"t":"fps","v":0}` 与 `{"t":"mode","reset":1}`。

---

## 8. 当前状态与已知限制

**当前状态**：集显路径串流稳定 1080p@60fps；所有功能正常；测试全绿；服务端已恢复默认参数。

**已知限制**：
- 光标形状非真实位图（前端画固定箭头）；`ext_image_copy_capture_cursor_session` 未接入。
- 自适应控制器仅 QP 单旋钮；AIMD 与丢包反馈闭环未接（`stats` 已上报，服务端未消费）。
- 4K 物理极限 ~17–23fps。
- 音频、剪贴板、文件传输不在本期范围。
- 该工作区**不是 git 仓库**，`git status` 不可用。

---

## 9. 待决策事项（方案 A：4K60）

是否投入**全 GPU 零拷贝管线**重写。路线：
1. `scripts/switch-gpu.sh nvidia` 把桌面迁独显（headless 已验证能创建，但 wwcap 是否真正用 nvenc 未最终确认）。
2. 抓帧改 screencopy dmabuf / EGL-dmabuf 直导，替代 shm 回读。
3. 色彩转换走 CUDA/NPP 或 NVENC 原生路径，全程留 GPU。
4. NVENC 直连显存编码（后端已就绪，但 `ww_enc_submit_cpu()` 是系统内存上传，需新增 dmabuf→CUDA 上传）。
5. 保留集显 `--enc vaapi` + shm 回退；两条路径互斥（seatd 同 seat 只允许一个 Hyprland）。

这是重写抓帧→转换→编码管线的工程，不是配置问题；是否推进需产品侧决策。

---

## 10. 环境 / 运行事实

- `XDG_RUNTIME_DIR=/run/user/1000`、`WAYLAND_DISPLAY=wayland-1`、Hyprland 实例签名取 `$XDG_RUNTIME_DIR/hypr/` 最新目录。
- 网关 `wwgw`（cwd 必须是项目根）静态托管 `web/`，HTTPS+WebTransport 均 :8443；前端 JS 改动即时生效，C 代码需 `cd capture/build && meson compile`。
- 抓帧命令：`wwcap --video /run/user/1000/ww-video.sock --ctrl /run/user/1000/wwcap-ctrl.sock --enc vaapi --output eDP-1 --max 3200x1800`，日志 `/tmp/wwcap.log`。
- 控制协议：Unix DGRAM `wwcap-ctrl.sock`，一条报文一条 JSON（如 `{"t":"kk","k":125,"d":1}`）。

**约束（用户明确要求，2026-11-10，已写入项目根 `AGENTS.md`）**：禁止用 curl/Python 脚本批量抓取网页或调 API 抓数据；必须用浏览器自动化（agent-browser/playwright/chrome-use）；同时打开的网页不超过 4 个。本地 e2e/CDP 验证属于自动化测试，与此不冲突。

**交接文档**：`/tmp/handoff-web-wayland.md`（临时目录，供新 agent 接手）。
