# web-wayland 测试用例集

本文档是测试用例的**人类可读说明**：按类别列出每个用例的前置条件、操作步骤、预期结果，
并给出对应的自动化代码位置。测试代码头部也反向链接到本文档对应章节，便于双向跳转。

> 单元/集成命名的约定：`E2E-*` 来自 [e2e-test.mjs](../scripts/e2e-test.mjs)，
> `REG-*` 来自 [regression-test.mjs](../scripts/regression-test.mjs)，
> `PERF-*` 来自 [perf-test.mjs](../scripts/perf-test.mjs)，
> `MAN-*` 为需要人工在真实浏览器中确认的用例。

## 目录

- [测试分层与运行方式](#测试分层与运行方式)
- [自测接口 window.\_\_ww](#自测接口-window__ww)
- [A. 环境与启动](#a-环境与启动)
- [B. 连接与传输](#b-连接与传输)
- [C. 画面渲染与清晰度](#c-画面渲染与清晰度)
- [D. 分辨率协商](#d-分辨率协商)
- [E. 输入链路](#e-输入链路)
- [F. 鼠标端到端测试](#f-鼠标端到端测试)
- [G. 设置面板与偏好](#g-设置面板与偏好)
- [H. 性能与延迟](#h-性能与延迟)
- [I. 稳定性与异常恢复](#i-稳定性与异常恢复)
- [J. 人工验证清单](#j-人工验证清单)
- [K. 音频](#k-音频)
- [L. 串流策略](#l-串流策略)
- [M. 色度采样 4:4:4](#m-色度采样-444)
- [N. 开机自启与空闲休眠](#n-开机自启与空闲休眠)
- [O. 原生客户端（wwclient）](#o-原生客户端wwclient)
- [S. Sunshine-only 部署验收（2026-11-21 起，人工）](#s-sunshine-only-部署验收2026-11-21-起人工)

---

## 测试分层与运行方式

| 层级 | 脚本 | 覆盖范围 | 说明 |
| --- | --- | --- | --- |
| 冒烟 | [e2e-test.mjs](../scripts/e2e-test.mjs) | 连接、分片、渲染、丢片、协商、解码队列 | 约 12s，失败即阻塞发版 |
| 回归 | [regression-test.mjs](../scripts/regression-test.mjs) | 冒烟 + 设置面板、编码器切换、HUD、输入、鼠标端到端、JS 错误 | 约 45s（含 14s 首帧等待与鼠标用例） |
| 性能 | [perf-test.mjs](../scripts/perf-test.mjs) | 四档分辨率下的前端 fps 实测 | 约 30s，输出表格供对比 |
| 手工 | 本文档 [J 节](#j-人工验证清单) | 鼠标手感、清晰度、多显示器等主观项 | 自动化环境无法覆盖 |

前置条件（所有自动化用例共用）：

1. 宿主机已执行 `bash scripts/run-all.sh`（或 `scripts/switch-gpu.sh intel|nvidia`），
   Hyprland、`wwgw`（:8443）、`wwcap` 均在运行。
2. 页面可通过 `https://127.0.0.1:8443/?auto=1&debug=1` 打开，且暴露 `window.__ww`。
3. 本机安装 `google-chrome`（脚本会自行拉起无头实例，用独立 profile 与 CDP 端口）。
4. 鼠标端到端用例（REG-MOUSE-01~06）还需要宿主机可执行 `hyprctl` 与 `foot`：
   脚本通过 `hyprctl cursorpos` / `activewindow` 断言远端光标与焦点窗口，并临时开合 `wwtest-*` 窗口。
   测试前请确认 compositor 会话健康（`hyprctl monitors` 有正常尺寸，屏幕不是安全模式降级画面）。

> 注意：IDE 终端可能注入私有 `LD_LIBRARY_PATH`，导致 Hyprland 报 GLIBCXX 缺失。
> 在 agent/IDE 终端里运行前请加 `env -u LD_LIBRARY_PATH`。

```bash
# 1) 拉起整套环境
env -u LD_LIBRARY_PATH bash scripts/run-all.sh

# 2) 冒烟（默认 1920x1080 窗口，CDP 9222）
env -u LD_LIBRARY_PATH node scripts/e2e-test.mjs

# 2b) 4K 协商场景：DPR=2 触发「物理 4K + scale 2」
WW_SCALE=2 WW_SIZE=1920,1080 env -u LD_LIBRARY_PATH node scripts/e2e-test.mjs 9223 16000

# 3) 全功能回归
env -u LD_LIBRARY_PATH node scripts/regression-test.mjs 9222 14000

# 4) 性能实测
env -u LD_LIBRARY_PATH node scripts/perf-test.mjs 9224
```

每个脚本末尾输出 `x/y 通过`，并以退出码 0/1 表示整体成败，可直接接 CI。
并行运行多个脚本时请给不同 CDP 端口（脚本会各自启动并回收无头 Chrome）。

## 自测接口 window.\_\_ww

页面内置的自测接口定义在 [main.js:363-380](../web/src/main.js#L363-L380)，
自动化用例只依赖它，不解析控制台日志。

| 方法 / 字段 | 含义 | 典型断言 |
| --- | --- | --- |
| `__ww.status().connected` | WebTransport 是否已连接且未关闭 | `=== true` |
| `__ww.status().datagrams` | 收到的 datagram 总数 | `> 100` |
| `__ww.status().lost` | QUIC 层丢片数 | `=== 0` |
| `__ww.status().rendered` | 已绘制到 canvas 的帧数 | 持续增长 |
| `__ww.status().resolution` | 服务端实际串流分辨率（如 `1920×1080`） | 与请求档位一致 |
| `__ww.status().requested` | 客户端协商出的请求模式 `{w,h}` | 用于对齐断言 |
| `__ww.status().decoderQueue` | VideoDecoder 待解码队列长度 | `<= 6` |
| `__ww.status().dropped` / `skipped` | 过期帧丢弃数 / 积压跳过数 | `dropped === 0` |
| `__ww.status().e2eMs` | 端到端延迟估计（ms） | 人工观察趋势 |
| `__ww.pointer(x, y)` | 归一化绝对坐标注入（0~1） | 服务端 cursorpos 校验 |
| `__ww.key(code, down)` | 键码注入 | 不抛异常、能触发快捷键 |
| `__ww.requestKeyframe()` | 主动请求 IDR | 花屏恢复 |

---

## A. 环境与启动

这一类用于确认整套串流环境健康，多为手工/脚本命令验证，不依赖浏览器。

| 编号 | 用例 | 前置 | 步骤 | 预期结果 | 对应位置 |
| --- | --- | --- | --- | --- | --- |
| ENV-01 | Hyprland 会话可用 | 已执行 `run-all.sh` | `hyprctl monitors -j` | 返回 JSON，含 HEADLESS 输出，且日志无 `--safe-mode` | [switch-gpu.sh](../scripts/switch-gpu.sh#L46-L68) |
| ENV-02 | headless 输出尺寸正确 | ENV-01 通过 | 查看 HEADLESS 的 `width/height/scale` | 与 `--max` 协商档位一致（intel 上限 3200×1800） | [run-all.sh:48-62](../scripts/run-all.sh#L48-L62) |
| ENV-03 | wwcap 单实例锁 | 已有 wwcap 在跑 | 再启动一个 `capture/build/wwcap` | 第二个进程立即退出，旧进程不受影响 | [main.c](../capture/src/main.c) 的 `flock` 分支 |
| ENV-04 | 抓帧帧率达标 | wwcap 运行中 | `grep 帧率 /tmp/wwcap.log` 或看 HUD | 稳定在 55~60fps（intel/vaapi） | [main.c](../capture/src/main.c) 的 `cap`/`conv` 耗时统计 |
| ENV-05 | 网关健康检查 | `wwgw` 运行中 | `curl -k https://127.0.0.1:8443/healthz` | `{"ok":true,...}`，含 `sessions_total` / `active` | [main.rs](../gateway/src/main.rs) 的 healthz 分支 |
| ENV-06 | WebTransport 证书有效期 | 重新签发证书后 | `openssl x509 -in certs/wt.crt -noout -dates` | 有效期 ≤ 14 天（Chrome 哈希钉扎硬性要求） | [gen-ca.sh:28-35](../scripts/gen-ca.sh#L28-L35) |

## B. 连接与传输

| 编号 | 用例 | 前置 | 步骤 | 预期结果 | 对应位置 |
| --- | --- | --- | --- | --- | --- |
| E2E-CONN-01 / REG-CONN-01 | 页面暴露自测接口 | 页面已加载 | 读取 `window.__ww` | 对象存在且 `status()` 可调用 | [e2e-test.mjs:72](../scripts/e2e-test.mjs#L72)、[regression-test.mjs:84](../scripts/regression-test.mjs#L84) |
| E2E-CONN-02 / REG-CONN-02 | WebTransport 已连接 | 同上 | `status().connected` | `true` | [e2e-test.mjs:74](../scripts/e2e-test.mjs#L74)、[regression-test.mjs:86](../scripts/regression-test.mjs#L86) |
| E2E-PIPE-01 / REG-PIPE-01 | 持续收到视频分片 | 连接建立后等待首帧 | `status().datagrams` | 冒烟 `> 5`；回归 `> 100` | [e2e-test.mjs:75](../scripts/e2e-test.mjs#L75)、[regression-test.mjs:87](../scripts/regression-test.mjs#L87) |
| E2E-PIPE-02 / REG-PIPE-02 | 零丢片 / 零解码错误 | 同上 | `status().lost`、`dropped` | 均为 `0` | [e2e-test.mjs:77](../scripts/e2e-test.mjs#L77)、[regression-test.mjs:89](../scripts/regression-test.mjs#L89) |
| CONN-05 | 多客户端输入归属（1.5s 租约） | 两个客户端同时连接 | A 先发输入，B 立刻再发；A 停止输入 >1.5s 后 B 再发 | 租约内仅 A 生效；租约过期后 B 接管光标；A 断开时若仍持有输入权则释放并下发 `keysreset` | [main.rs](../gateway/src/main.rs) 的 `INPUT_OWNER` / `INPUT_LAST` / `INPUT_LEASE_MS` |
| CONN-06 | 断线重连 | 串流中 kill wwcap 再重启 | 观察页面 | 自动回到连接面板并可重新进入，不残留旧 transport/decoder | [main.js](../web/src/main.js) 的 `resetSession` |

## C. 画面渲染与清晰度

| 编号 | 用例 | 前置 | 步骤 | 预期结果 | 对应位置 |
| --- | --- | --- | --- | --- | --- |
| E2E-RENDER-01 / REG-PIPE-01 | 已渲染帧数 | 连接后等待 12s/14s | `status().rendered` | 冒烟 `>= 10`；回归 `> 100` | [e2e-test.mjs:76](../scripts/e2e-test.mjs#L76)、[regression-test.mjs:87](../scripts/regression-test.mjs#L87) |
| REG-RENDER-01 | canvas 尺寸等于串流分辨率 | 已出图 | 读 `#screen` 的 `width/height` | `>= 1920×1080`，且不超过 DPR 上限 | [regression-test.mjs:92-108](../scripts/regression-test.mjs#L92-L108) |
| REG-RENDER-02 | 画面非黑屏 | 已出图 | 在 canvas 上取 5 个采样点算亮度均值/方差 | `mean > 8` 且 `variance > 100` | [regression-test.mjs:108](../scripts/regression-test.mjs#L108) |
| E2E-DEC-01 | 解码队列未堆积 | 稳定串流中 | `status().decoderQueue` | `<= 6`（超阈值会主动丢帧请求关键帧） | [e2e-test.mjs:97](../scripts/e2e-test.mjs#L97) |
| E2E-DEC-02 | 无解码错误 | 同上 | `status().dropped` + 控制台 decode 错误 | `dropped === 0` 且无 decode 关键字报错 | [e2e-test.mjs:99](../scripts/e2e-test.mjs#L99) |
| REG-ART-01 | 最终截图有效 | 串流正常 | `Page.captureScreenshot` 保存 PNG | 文件 `> 20000 bytes`（排除纯色空屏） | [regression-test.mjs:202-203](../scripts/regression-test.mjs#L202-L203) |

> 清晰度相关修复的回归点：`fitCanvas` 不会按 DPR 放大 canvas（见 [main.js](../web/src/main.js)），
> 解码器如实上报 SPS 的 profile/level（见 [decoder.js](../web/src/decoder.js)），
> 这两处退化会分别表现为「画面发糊」和「部分机器直接黑屏」。

## D. 分辨率协商

协商规则：客户端按可用区域与 DPR 算出请求档位（1920/2560/3200/3840），服务端**向下取档**，
再通过 `hyprctl keyword monitor <OUT>,WxH@60,0x0,scale` 调整 headless 输出。

| 编号 | 用例 | 前置 | 步骤 | 预期结果 | 对应位置 |
| --- | --- | --- | --- | --- | --- |
| E2E-RES-01 | 4K + scale 2 协商 | `WW_SCALE=2` 启动 | 等待编码器重建，轮询 `status().resolution` | 以 `3` 开头（3840 档） | [e2e-test.mjs:80-87](../scripts/e2e-test.mjs#L80-L87) |
| E2E-RES-02 | 分辨率与请求一致 | 默认 DPR=1 | 比较 `status().resolution` 与 `requested` | 宽高误差 `<= 1`（取偶） | [e2e-test.mjs:88-95](../scripts/e2e-test.mjs#L88-L95) |
| PERF-RES | 四档逐级切换 | 串流正常 | 依次切 1920/2560/3200/3840 并采样 | 每档实际分辨率与请求一致，fps 不归零 | [perf-test.mjs:50-61](../scripts/perf-test.mjs#L50-L61) |
| RES-04 | 非标准请求向下取档 | 面板选 3200 档 | 观察 `resolution` | 取到不超过上限的最大档（intel 为 3200×1800） | [main.js](../web/src/main.js) 的个人档位映射 |

## E. 输入链路

输入经 `wwgw` → wwcap 虚拟键鼠注入 Hyprland。本类覆盖按键与自测接口的坐标注入；
鼠标的真实端到端行为（跟随、点击、错位）单独见 [F. 鼠标端到端测试](#f-鼠标端到端测试)。

| 编号 | 用例 | 前置 | 步骤 | 预期结果 | 对应位置 |
| --- | --- | --- | --- | --- | --- |
| E2E-INPUT-01 | 绝对坐标注入 | 串流正常 | `__ww.pointer(0.25,0.25)` → `(0.75,0.6)`，再截图 | 无异常，光标出现在对应位置 | [e2e-test.mjs:101-109](../scripts/e2e-test.mjs#L101-L109) |
| E2E-INPUT-02 / REG-INPUT-01 | 按键注入无异常 | 同上 | `__ww.key(...)` 注入组合键 | 不抛错，服务端可收到 | [e2e-test.mjs:103](../scripts/e2e-test.mjs#L103)、[regression-test.mjs:330-337](../scripts/regression-test.mjs#L330-L337) |
| MAN-INPUT-01 | 指针「跟手」 | 真实浏览器 | 快速画圈、缓慢精确定位 | 无拖影/无累积延迟，绝对模式 1:1 跟随 | [input.js](../web/src/input.js) |
| MAN-INPUT-02 | 点击不再强制锁定指针 | 真实浏览器 | 单击画面后移动鼠标 | 光标仍按绝对坐标移动，不进入 Pointer Lock | [input.js](../web/src/input.js) 的 `?lock=1` 分支 |
| MAN-INPUT-03 | 画布外操作 | 真实浏览器 | 光标移出画面/移入黑边 | 不发 `pm`，不发生坐标 clamp 到边缘 | [input.js](../web/src/input.js) 的 `#flush` |
| MAN-INPUT-04 | 失焦释放按键 | 真实浏览器 | 按住 Shift/方向键时切走窗口 | 远端按键全部释放，不出现「粘键」 | [input.js](../web/src/input.js) 的 `releaseAll` |
| MAN-INPUT-05 | CapsLock / 修饰键 | 真实浏览器 | 按 CapsLock 后输入字母 | 大小写状态正确，失焦恢复后不残留 | [input.c](../capture/src/input.c) 的 `locked` |
| MAN-INPUT-06 | 屏幕上只有一支光标 | 真实浏览器 | 在视频区域内移动、点击 | **2026-11-20 起反转**：视频里已经没有光标（服务端透明主题），只有客户端自绘的 SVG 箭头；若看到两支说明 `/capabilities` 的 `cursor` 与实际主题不一致 | [main.js](../web/src/main.js) 的 `enableLocalCursor`；`config/hypr/hyprland.conf` 的 `enable_hyprcursor=false` |
| MAN-INPUT-07 | 本地光标延迟 | 真实浏览器 | 快速拖动鼠标 | 箭头立即跟手（不再有 26~50ms 拖影）；移出画布即隐藏、交还系统光标 | `#cursor-local` + `input.js` 的 `onPointer`；追踪报告「光标可见延迟」应 ≈0 |
| MAN-INPUT-08 | 抓帧里确实没有光标 | 服务器本地 | `grim -o HEADLESS-2` 在指针两个不同位置各截一帧 | 两帧**差异像素为 0**（有光标时约 150px/位置）| `hyprctl setcursor` 运行时不生效，必须 `XCURSOR_THEME` + 重启会话 |

> 多鼠标说明：Hyprland 是单光标模型，宿主机物理鼠标与串流指针共用同一光标，
> 因此「多个鼠标互不干扰」目前定义为**多客户端输入归属隔离**（CONN-05），非独立光标。

## F. 鼠标端到端测试

用 CDP 派发真实 DOM 鼠标事件，走完整链路：
浏览器 `input.js` → 网关 → wwcap 虚拟指针 → Hyprland。
宿主机侧用 `hyprctl cursorpos` / `activewindow` 断言光标位置与点击命中，覆盖「流畅、能点、不错位」。

| 编号 | 用例 | 前置 | 步骤 | 预期结果 | 对应位置 |
| --- | --- | --- | --- | --- | --- |
| REG-MOUSE-01 | 移动位置准确（无错位） | 串流正常，能读到 `hyprctl` 与 wwcap 的输出名 | 按归一化坐标 `(0.25,0.25)`、`(0.7,0.6)`、`(0.5,0.5)` 派发 `mouseMoved`，轮询 `hyprctl cursorpos` | 光标落在 `mon.x + nx×mon.width` 附近，误差 ≤6px；多显示器下也不偏移 | [regression-test.mjs:123-154](../scripts/regression-test.mjs#L123-L154)、[input.c](../capture/src/input.c) 的 `motion_absolute` 布局换算 |
| REG-MOUSE-02 | 移动响应及时（不卡顿） | 同上 | 记录每次移动从派发到光标命中目标的耗时 | 最慢 <400ms（实测 20~35ms） | [regression-test.mjs:155](../scripts/regression-test.mjs#L155) |
| REG-MOUSE-03 | 连续移动无停顿 | 同上 | 在画布内水平扫 30 个点（每点间隔 16ms），中途抽样 `cursorpos.x` | x 单调推进（允许 4px 抖动） | [regression-test.mjs:156-169](../scripts/regression-test.mjs#L156-L169) |
| REG-MOUSE-04 | 连续移动到达终点 | 同上 | 扫点结束后读 `cursorpos` | 与终点误差 ≤8px（卡死会停在中途） | [regression-test.mjs:170](../scripts/regression-test.mjs#L170) |
| REG-MOUSE-05 | 点击命中左右窗口 | 测试自动用 `foot -T wwtest-left/right` 开两个窗口 | 依次点击两窗口中心，读 `hyprctl activewindow -j` | 焦点窗口 address 与点击目标一致（点击位置没有错位） | [regression-test.mjs:171-199](../scripts/regression-test.mjs#L171-L199) |
| REG-MOUSE-06 | 快速移动后点击不落到旧位置 | 同上 | 同一帧内连续派发 `mouseMoved`→`mousePressed`→`mouseReleased` 到右侧窗口 | 焦点仍是右侧窗口：`mousedown` 前已把合批坐标 flush 出去 | [regression-test.mjs:201-207](../scripts/regression-test.mjs#L201-L207)、[input.js](../web/src/input.js) 的 `#setAbs` + `#flush` |

## G. 设置面板与偏好

| 编号 | 用例 | 前置 | 步骤 | 预期结果 | 对应位置 |
| --- | --- | --- | --- | --- | --- |
| REG-SET-01 | 设置面板开合 | 页面已加载 | 点击齿轮两次 | 面板显隐状态切换 | [regression-test.mjs:111-119](../scripts/regression-test.mjs#L111-L119) |
| REG-SET-02 | 帧率切换到 30 | 同上 | `#set-fps` 改为 30 并派发 change | `localStorage['ww.pref'].fps === 30` 且实际降帧 | [regression-test.mjs:121-125](../scripts/regression-test.mjs#L121-L125) |
| REG-SET-03 | 切换到 NVENC | 同上 | `#set-enc` 选 `nvenc` | 偏好写入 `nvenc`，编码器重建 | [regression-test.mjs:128-131](../scripts/regression-test.mjs#L128-L131) |
| REG-ENC-01 | NVENC 切换后持续渲染 | 同上 | 对比切换前后 `rendered` | `rendered` 继续增长（允许短暂卡顿） | [regression-test.mjs:132-133](../scripts/regression-test.mjs#L132-L133) |
| REG-SET-04 | 切回 VAAPI | 同上 | `#set-enc` 选 `vaapi` | 偏好写入 `vaapi`，画面恢复 | [regression-test.mjs:135-138](../scripts/regression-test.mjs#L135-L138) |
| REG-SET-05 | HUD 开关 | 同上 | 取消/勾选 `#set-hud` | `#hud` 同步隐藏/显示 | [regression-test.mjs:140-148](../scripts/regression-test.mjs#L140-L148) |
| SET-06 | 编码器默认跟随服务端 | 首次访问 | 打开设置查看编码器选项 | 默认空值「跟随服务端」，老值 `vaapi` 自动清空 | [index.html](../web/index.html)、[main.js](../web/src/main.js) 的 `pref.enc` |
| SET-07 | 显示方式切换 | 画面已出图 | 设置里把「显示方式」在铺满/原始像素之间切换 | 铺满时画面填满窗口（可放大）；原始像素时 1 流像素对应 1 设备像素、四周留黑边；两者均写入偏好 | [main.js](../web/src/main.js) 的 `fitCanvas` |
| REG-SET-06 | 统计信息默认关闭 | 页面已加载 | 观察 `#stats` 与 `ww.pref.stats` | 面板隐藏，偏好不为 `true` | [regression-test.mjs:150-155](../scripts/regression-test.mjs#L150-L155) |
| REG-SET-07 | 统计信息开关与实时数据 | 同上 | 勾选 `#set-stats`，等 2.6s | 面板显示 Video stream/FPS 等实时值；`status().rttMs` 为数字 | [regression-test.mjs:156-170](../scripts/regression-test.mjs#L156-L170) |
| REG-SET-08 | 网关 ping/pong RTT | 统计面板可见 | 每秒发送 `{t:'ping'}` | 网关直接回 `pong`，文本形如 `x ms (variance: y ms)` | [regression-test.mjs:169-170](../scripts/regression-test.mjs#L169-L170)、[main.rs](../gateway/src/main.rs) 的 pong 分支 |
| REG-SET-09 | 快捷键帮助 | 同上 | 点 `#set-help` 再点 `#help-close` | 弹层打开并可关闭 | [regression-test.mjs:172-180](../scripts/regression-test.mjs#L172-L180) |
| SET-10 | 不可测统计字段 | 统计面板可见 | 查看 Host processing latency | 显示 `- / - / - ms`；原因见 [README 约束 9](../README.md#关键约束实测得出改动前务必阅读) | [index.html](../web/index.html)、[main.js](../web/src/main.js) 的 `updateStats` |
| MAN-SET-11 | Ctrl+Alt+F1 切换统计 | 串流中 | 按热键两次 | 面板显隐，设置里的「统计信息」复选框同步 | [input.js](../web/src/input.js)、[main.js](../web/src/main.js) 的 `setStatsVisible` |

## H. 性能与延迟

| 编号 | 用例 | 前置 | 步骤 | 预期结果 | 对应位置 |
| --- | --- | --- | --- | --- | --- |
| PERF-01 | 四档分辨率 fps | 串流正常 | 逐档切换，每档等 6s 后采样 3 次取均值 | 1080p ≥ 55fps；4K ≥ 30fps（intel 软/硬编实测值记录在案） | [perf-test.mjs:50-61](../scripts/perf-test.mjs#L50-L61) |
| PERF-02 | 端到端延迟 | 同上 | 读 `status().e2eMs`，与 HUD 对比 | 局域网内稳定在低延迟区间，无持续增长趋势 | [main.js:370](../web/src/main.js#L370) |
| PERF-03 | 解码积压主动丢帧 | 构造慢解码 | 观察 `skipped` 与 `decoderQueue` | 队列超 3 时丢 delta 帧并请求关键帧，队列回落 | [decoder.js](../web/src/decoder.js) 的 `push` |

性能实测命令：`env -u LD_LIBRARY_PATH node scripts/perf-test.mjs 9224`，输出形如：

```text
1920x1080  -> 实际 1920×1080  前端fps≈59.8  rendered=1234
2560x1440  -> 实际 2560×1440  前端fps≈59.5  rendered=1102
3200x1800  -> 实际 3200×1800  前端fps≈40.1  rendered=780
3840x2160  -> 实际 3200×1800  前端fps≈40.0  rendered=760
```

（最后一档在 intel 路径会被上限截到 3200×1800，属预期行为。）

## I. 稳定性与异常恢复

| 编号 | 用例 | 前置 | 步骤 | 预期结果 | 对应位置 |
| --- | --- | --- | --- | --- | --- |
| REG-ERR-01 | 无 JS 运行时错误 | 全流程结束 | 收集 console error / exception | 过滤 favicon、证书、WebTransport close 后为空 | [regression-test.mjs:159-161](../scripts/regression-test.mjs#L159-L161) |
| STAB-01 | 编码器重建失败退避 | 强制指定不可用编码器 | 观察 wwcap 日志 | 1s 退避后重试，不忙等、不退出 | [main.c](../capture/src/main.c) 的 `enc_retry_at` |
| STAB-02 | 网关 HTTPS 错误不退出 | 持续用无效握手连接 | 循环建立 TCP 连接 | 每次失败 sleep 100ms 后继续 accept | [main.rs](../gateway/src/main.rs) 的 accept 循环 |
| STAB-03 | 路径穿越拦截 | 网关运行中 | 请求 `/../etc/passwd` | 返回 403/404，不读出文件 | [main.rs](../gateway/src/main.rs) 的路径校验 |
| STAB-04 | wwcap 优雅退出 | wwcap 运行中 | `kill -TERM <pid>` | 释放虚拟键鼠、清理 shm 与 ctrl socket，Hyprland 存活 | [main.c](../capture/src/main.c) 的 `on_signal` |
| STAB-05 | Hyprland 崩溃恢复 | 会话崩溃后 | 重新执行 `switch-gpu.sh` | 新会话可正常出图，`fps > 0` | [switch-gpu.sh](../scripts/switch-gpu.sh) |
| STAB-06 | 客户端重启流服务 | 串流中（画面卡住/黑屏但连接还在） | 设置 → 串流控制 →「重启采集」 | wwcap 进程 PID 变化，约 1–3s 内帧计数恢复增长，该行显示「已恢复（x.xs）」；5s 内重复点击返回 `busy` | [restart-capture.sh](../scripts/restart-capture.sh)、[main.js](../web/src/main.js) 的 `requestRestart` |
| STAB-07 | 强制恢复桌面 | 重启采集后仍不出图 | 点「强制恢复桌面」并确认 | 桌面会话重建（桌面里的窗口会关闭），客户端刷新后可重新进入；`/tmp/ww-restart.log` 有 run-all 输出 | [restart-capture.sh](../scripts/restart-capture.sh) 的 `--deep` |
| STAB-08 | 重启节流 | 同上 | 连点二次「重启采集」 | 第二次立即回「刚重启过，等 5 秒再试」，不会叠加多个 wwcap | [main.rs](../gateway/src/main.rs) 的 `RESTART_HOLD_MS` |

## J. 人工验证清单

以下项目依赖真实浏览器与主观体感，自动化环境（隔离网络 + 自签证书）无法覆盖，
请在**已信任本地 CA 的 Chrome** 中按顺序确认：

| 编号 | 场景 | 操作 | 通过标准 |
| --- | --- | --- | --- |
| MAN-01 | 首次连接 | 打开 `https://<局域网IP>:8443/`（或 `?auto=1`） | 3s 内出图，无证书警告导致的中断 |
| MAN-02 | 鼠标跟手 | 快速画圈 10 次、缓慢定位到图标 | 无「甩尾/回弹」，点击位置与光标一致 |
| MAN-03 | 拖拽选择 | 按住左键框选一段文本 | 选区和指针同步，不中断 |
| MAN-04 | 滚轮 | 在长页面连续滚动 | 滚动方向与步长正常，无跳变 |
| MAN-05 | 键盘 | 输入中文/英文，使用 Ctrl+C/V | 输入法与剪贴板快捷键正常 |
| MAN-06 | 切屏/失焦 | 切到别的窗口再回来 | 无粘键，回来后仍可操作 |
| MAN-07 | 清晰度 | 查看 12~16px 小字、细线图标 | 文字边缘清晰，不发糊/不糊成块 |
| MAN-08 | 分辨率切换 | 面板切 2560/3200/3840 | 画面在 2~3s 内重建完成，比例正确无黑边 |
| MAN-09 | 编码器切换 | 面板切 NVENC/VAAPI | 切换期间短暂卡顿后恢复，HUD 显示编码器正确 |
| MAN-10 | 断线恢复 | 断开网络 3s 后恢复 | 显示连接面板，点击可重新进入 |
| MAN-11 | 多客户端 | 两个浏览器标签同时连接 | 后连接一方画面正常但输入不抢占；先断开一方后另一方仍可用 |
| MAN-12 | 长时间运行 | 连续使用 30 分钟 | 延迟不逐分钟增长，内存/解码队列稳定 |

---

## K. 音频

音频通路（与视频独立计数、共用 datagram 通道）：

```
PipeWire 虚拟 sink ww-desk 的 monitor（只含应用声音）──▶ wwcap（Opus 48k/立体声/20ms，每包带 seq-1 做 RED）
   ──▶ ww-audio.sock ──▶ wwgw ──▶ WebTransport datagram（首字节 = 2）
   ──▶ 浏览器 AudioDecoder ──▶ AudioWorklet 环形缓冲（初始 40ms）──▶ 扬声器
```

| 编号 | 用例 | 前置 | 步骤 | 预期结果 | 对应位置 |
| --- | --- | --- | --- | --- | --- |
| ENV-AUD-01 | 音频依赖齐备 | Debian 13 | `pkg-config --exists libpipewire-0.3 opus`（两者都要成功）；`wpctl status` 看默认 sink | 命令返回 0；默认 sink（带 `*`）是 `web-wayland 桌面音频`（ww-desk） | [meson.build](../capture/meson.build)、[50-ww-desk.conf](../scripts/pipewire/50-ww-desk.conf) |
| ENV-AUD-02 | 采集启动 | wwcap 带 `--audio` 启动 | `grep '\[aud\]' /tmp/wwcap.log`；`pw-link -l \| grep wwcap-audio` | 依次出现「采集已启动」「PipeWire monitor 已连上」，每秒 `pkt/s ≈ 50`；输入只来自 `ww-desk:monitor_FL/FR` | [audio.c](../capture/src/audio.c)、[main.c](../capture/src/main.c) 的 `--audio` |
| ENV-AUD-03 | 缺依赖时降级为无声 | 手动把 `--audio` 指向不存在的设备/停掉 PipeWire | 重启 wwcap | 日志打「初始化失败，串流将没有声音」，**画面仍正常** | [main.c](../capture/src/main.c) 的 `ww_audio_create` 失败分支 |
| K-01 | 下行链路出声 | 已连接、我的音量 > 0、未勾「静音目标机」 | `bash scripts/audio-test-tone.sh 5` | 浏览器 5s 内听到 1kHz 连续音，无断续 | [audio-test-tone.sh](../scripts/audio-test-tone.sh) |
| K-02 | 无人观看不采集 | 没有浏览器连接 | 观察 wwcap 日志 10s | `[aud] ... pkt/s` 为 0；连上后恢复 ≈50 | [main.c](../capture/src/main.c) 的 `astream` 分支、网关会话建立/结束 |
| K-03 | 抖动缓冲稳定 | 串流中持续放音 | 打开设置看「音频状态」 | 缓冲稳定在 40–80ms，不逐分钟增长；`ctx.state = running` | [audio.js](../web/src/audio.js)、[audio-worklet.js](../web/src/audio-worklet.js) |
| K-04 | 单包丢失可恢复 | 弱网或人为丢包 | 观察状态里「补帧 / 丢帧」计数并试听 | 补帧增长时**听不出断音**；连续丢包只有轻微淡出，无爆音 | [audio.js](../web/src/audio.js) 的 `push` |
| K-05 | 静音目标机与恢复 | 串流中 | 勾选「串流开始时静音目标计算机扬声器」→ `wpctl get-volume @DEFAULT_AUDIO_SINK@` → 断开全部客户端 | 连接后显示 `MUTED`；最后一个客户端断开或 wwcap 退出后恢复原状态 | [main.c](../capture/src/main.c) 的 `sink_mute_set` / `sink_mute_restore` |
| K-06 | 失焦静音 | 勾选「失去焦点时静音」 | 切到其他窗口再切回 | 切走时无声、切回恢复；目标机 sink 状态不受影响 | [main.js](../web/src/main.js) 的 `blur`/`focus` 监听 |
| K-07 | 重连音频重建 | 串流中断开再进入 | 观察「音频状态」并试听 | 状态回到 running、缓冲重新收敛到 40–80ms，无持续杂音 | [main.js](../web/src/main.js) 的 `resetSession` + `startAudio` |
| K-08 | 麦克风/环境声不进入串流 | 默认 sink 为 ww-desk、麦克风未静音 | `pw-link -l \| grep wwcap-audio`；对着服务器麦克风说话 5s，观察客户端与 `grep '\[aud\]' /tmp/wwcap.log` | 输入只来自 `ww-desk:monitor_FL/FR`；客户端**听不到说话声**，服务器扬声器也无回环蜂鸣 | [50-ww-desk.conf](../scripts/pipewire/50-ww-desk.conf)、[audio.c](../capture/src/audio.c) 的 `WW_AUDIO_TARGET_NODE` |
| REG-AUD-01 | 音频包到达浏览器 | `regression-test.mjs` 运行中 | 读 `status().audioPackets` | `> 0`（服务端在放静音帧也算，证明链路通） | [regression-test.mjs](../scripts/regression-test.mjs) |
| REG-AUD-02 | Opus 解码产出 PCM | 同上 | 读 `status().audioDecoded` | `> 0`（解码器配置正确、`description` 合法） | [audio.js](../web/src/audio.js) 的 `#onData` |
| MAN-AUD-01 | 主观音质 | 真实浏览器 + 音箱 | 放音乐/视频 5 分钟 | 无金属音、无周期性咔哒（有咔哒说明水位/时钟有问题）；可容忍 64kbps 的细节损失 | — |
| MAN-AUD-02 | 唇音同步 | 播放带对话的视频 | 观察口型与声音 | 偏差 < 120ms（音频为主时钟；二期再做视频追帧微调） | [audio.js](../web/src/audio.js) |

> 排查顺序：`wpctl status` 看默认 sink 是否 `ww-desk`、`pw-link -l | grep wwcap-audio` 确认只连它 →
> `grep '\[aud\]' /tmp/wwcap.log` 看 wwcap 是否在发
> → `curl -k https://<IP>:8443/healthz` 看 `audio_in/audio_out` 是否增长 → 再看浏览器设置里的「音频状态」。

---

## L. 串流策略

三种策略是**组合预设**，只控制 QP（量化参数）与目标帧率，**永不修改分辨率**。
分辨率始终由「分辨率」下拉决定：自动档仍走原有基于帧率的升降档，手选档锁死。

| 编号 | 用例 | 前置 | 步骤 | 预期结果 | 对应位置 |
| --- | --- | --- | --- | --- | --- |
| STRAT-01 | 默认平衡 | 首次打开 / 清空 `ww.pref` | 打开设置面板 | 「平衡」高亮；画质为 QP 20；帧率下拉为 自动/30/45/60 | [main.js](../web/src/main.js) 的 `STRATEGY_QP`、[main.c](../capture/src/main.c) 的 `WW_STRAT_BALANCE` |
| STRAT-02 | 流畅优先 | 已连接 | 点「流畅优先」 | 帧率下拉只剩 30/60 并切到 60；日志 `策略 -> 流畅优先（QP 26`；实测 < 85% 目标时 QP 逐级升到 30，> 95% 时降到 24 | [main.c](../capture/src/main.c) 的 `apply_strategy` / 1 秒统计块 |
| STRAT-03 | 画质优先 | 已连接（可 4K） | 点「画质优先」 | QP 固定 16；日志 `策略 -> 画质优先`；帧率不被强制，4K 下可能约 20fps | [main.c](../capture/src/main.c) 的 `strategy_preset_qp` |
| STRAT-04 | 手动 QP 切自定义 | 任意策略 | 拖动画质滑块并松手 | 策略状态变「自定义」；服务端日志 `QP -> n（自定义）`，之后策略不再覆盖 QP | [main.js](../web/src/main.js) 的 `setQp` input；[main.c](../capture/src/main.c) 的 `qp` 分支 |
| STRAT-05 | 策略不改分辨率 | 手选 2560×1440 | 依次切换三种策略 | 分辨率下拉保持 2560×1440；`hyprctl monitors` 模式不变；统计面板分辨率不变 | [main.c](../capture/src/main.c)（`apply_strategy` 不调用 `apply_preset`） |
| REG-STRAT-01 | 策略状态可观测 | `regression-test.mjs` 运行中 | 读 `status().strategy` | 默认 `0`；点按钮后与服务端一致 | [main.js](../web/src/main.js) 的 `window.__ww.status` |

## M. 色度采样 4:4:4

4:4:4 是**独立开关**，与三种策略无关。开启后 H.264 profile 从 High(100) 变为
High 4:4:4 Predictive(244)，文字/红蓝边缘更锐利，码率约 +30~50%。
仅 NVIDIA NVENC + yuv444p 可用；Intel VAAPI 不支持。

| 编号 | 用例 | 前置 | 步骤 | 预期结果 | 对应位置 |
| --- | --- | --- | --- | --- | --- |
| CHROMA-01 | 能力探测与置灰 | 集显（VAAPI）会话 | 打开设置面板 | 开关置灰，原因「集显 VAAPI 不支持」；`curl -k https://127.0.0.1:8443/capabilities` 返回 `chroma444:0` | [main.c](../capture/src/main.c) 的 `chroma_refresh`、[main.rs](../gateway/src/main.rs) 的 `/capabilities` |
| CHROMA-02 | NVENC 开启 4:4:4 | 独显会话且试开 yuv444p 成功 | 勾选开关 | 日志「色度采样 -> 4:4:4」「h264_nvenc ... 色度=4:4:4」；统计面板显示 `H.264 High 4:4:4 · 4:4:4` | [encoder_vaapi.c](../capture/src/encoder_vaapi.c) 的 `create_nvenc`、[main.js](../web/src/main.js) 的 `chromaName` |
| CHROMA-03 | 切集显自动回退 | 4:4:4 已开启 | 编码器切到 vaapi | 自动回退 4:2:0 并保留偏好；开关置灰并提示「切回支持的编码器后自动恢复」；切回 nvenc 自动恢复 4:4:4 | [main.c](../capture/src/main.c) 的 `enc` 分支 + `chroma_refresh` |
| CHROMA-04 | 浏览器不支持解码 | 不支持 4:4:4 的浏览器 | 打开设置面板 | 开关置灰，原因「当前浏览器不支持 4:4:4 解码」 | [main.js](../web/src/main.js) 的 `probeDecode444` |
| CHROMA-05 | 码率提示 | — | 查看开关下方说明 | 文案包含「码率约 +30~50%」 | [index.html](../web/index.html) 的 `set-chroma-hint` |
| CHROMA-06 | 即时生效 | 串流中 | 勾选 / 取消 4:4:4 | 下一帧起重建编码器并强制 IDR，画面自动恢复，无需刷新页面 | [main.c](../capture/src/main.c) 的 `encoder_restart` |
| REG-CHROMA-01 | 色度可观测 | `regression-test.mjs` 运行中 | 读 `status().chroma` | `4:2:0` 或 `4:4:4`，与 SPS profile 一致 | [main.js](../web/src/main.js) 的 `window.__ww.status` |

---

## N. 开机自启与空闲休眠

服务由 systemd 用户单元常驻：`ww-session`（Hyprland）→ `ww-gateway`（:8443）→ `ww-cap`（抓帧编码）。
网关在会话建立/最后一个会话断开时向 wwcap 发 `{"t":"active","v":1|0}`；无会话 30 秒后 wwcap 停止抓帧并销毁编码器，新连接立即唤醒。

| 编号 | 用例 | 前置 | 步骤 | 预期结果 | 对应位置 |
| --- | --- | --- | --- | --- | --- |
| BOOT-01 | 开机自启已启用 | 已执行 `scripts/install-services.sh` | `systemctl --user is-enabled ww-session ww-gateway ww-cap`；`loginctl show-user $USER -p Linger` | 三个单元均为 `enabled`，`Linger=yes`（无人登录也会随开机拉起） | [install-services.sh](../scripts/install-services.sh) |
| BOOT-02 | 无会话自动休眠 | 已连过又断开全部客户端 | 等 30 秒后看 `journalctl --user -u ww-cap -n 5`，并测 CPU | 日志「无会话 30 秒，进入休眠（已停抓帧/编码）」；进程 CPU 接近 0，不再刷 fps 日志 | [main.c](../capture/src/main.c) 的主循环 `suspended` 判定 |
| BOOT-03 | 新会话唤醒 | wwcap 已休眠 | 浏览器连上 | 日志「客户端会话接入」「会话接入，恢复采集」；首帧为 IDR，约 1 秒出图 | [main.rs](../gateway/src/main.rs) 的 `active` 通知、[main.c](../capture/src/main.c) 的 `active` 分支 |
| BOOT-04 | 桌面配置生效 | systemd 启动的会话 | `hyprctl monitors -j`；`hyprctl dispatch exec foot` | HEADLESS 输出 1920x1080 scale 1.0；能创建窗口（说明加载的是项目配置，不是 `/usr/share/hypr/hyprland.lua`） | [ww-session.service](../scripts/ww-session.service) 的 `--config` |

---

## O. 原生客户端（wwclient）

原生客户端走网关新增的 UDP 通道 `:8444`（与浏览器 `:8443` 并行），复用同一套 24 字节分片与
同一套输入 JSON。**它不依赖浏览器，也不依赖合成器**：测试流可以由
[wwsrc](../native/wwsrc.c) 直接注入 `ww-video.sock`，因此在合成器故障时仍可完整验证
「分片 → 重组 → 解码 → 上屏 → 输入」这条链路。

收益数据与对照结论见 [原生客户端收益评估](./native-client-assessment.md)。

前置条件：

1. 网关在跑且监听 `:8444/udp`（`ss -lunp | grep 8444`）。
2. `native/` 已 `make`，且**要么** wwcap 在抓帧 **要么** 用 `wwsrc` 喂离线流（真机抓帧坏掉时的替代）。
3. `env -u LD_LIBRARY_PATH`（IDE 终端注入了私有 libstdc++）。

```bash
cd native
./wwsrc --file /tmp/clip.h264 --fps 60 &                 # 合成源（与 wwcap 同分片格式）
./wwclient --host 127.0.0.1 --port 8444 --no-present --bench 10 --stats-json /tmp/s.json
SDL_VIDEODRIVER=offscreen ./wwclient --size 1920x1080 --no-vsync --dump-gl /tmp/gl.ppm
```

| 编号 | 用例 | 前置 | 步骤 | 预期结果 | 对应位置 |
| --- | --- | --- | --- | --- | --- |
| NAT-01 | HELLO 握手与能力回报 | 网关运行 | 启客户端，看 stderr | 1 s 内打印 `服务端就绪: {"t":"hello_ack",...}`，含 `chroma444/encoder/color/range` | [native.rs](../gateway/src/native.rs) 的 `T_HELLO` 分支、[wwclient.c](../native/wwclient.c) 的 `T_HELLO_ACK` |
| NAT-02 | 分片重组完整 | 稳定流 10 s | `--stats-json` 读 `bad_frags` | `bad_frags === 0`，`frames` 与源端发送帧数一致（±1） | [wwclient.c](../native/wwclient.c) 的 `reasm_frag` |
| NAT-03 | 满帧与延迟预算 | 1080p60 源 | `--no-present --bench 10` | `fps ≥ 59`，`decode_p50 < 8 ms`，`e2e_p50 < 15 ms`（同机时钟） | [wwclient.c](../native/wwclient.c) 的 `summary` 输出 |
| NAT-04 | 丢片 → kf → IDR 恢复 | `wwsrc --drop 0.1` | 观察 `dropped/skipped/undecodable` 与源端日志 | 有 `dropped` 增长；`undecodable === 0`（不会把花屏送去解码）；恢复后 fps 回升 | [wwclient.c](../native/wwclient.c) 的 `request_keyframe` + `g_need_key`、[wwsrc.c](../native/wwsrc.c) 控制口 |
| NAT-05 | 4:4:4 / 全范围 / 709 色彩 | 444 全范围源 | `--dump-frame` 落帧，与参考帧 PSNR/ΔE | PSNR ≥ 48 dB，ΔE2000 平均 < 1.0（实测 51.96 dB / 0.126） | [wwclient.c](../native/wwclient.c) 的 `dump_ppm` |
| NAT-06 | GL 上屏色彩等价 | offscreen 驱动可用 | `--dump-gl` 读回 framebuffer 后与参考帧比对 | 与 NAT-05 同量级（实测同为 51.96 dB），通道偏差 < 1/255 | [wwclient.c](../native/wwclient.c) 的 `gl_readback` |
| NAT-07 | 上屏耗时可测 | offscreen 或真实窗口 | 读 `present_p50_ms / p95` | p50 < 4 ms（无 vsync 的乐观值，报告须注明） | [wwclient.c](../native/wwclient.c) 的 `gl_present` |
| NAT-08 | 输入注入 | 客户端窗口有焦点 | 移动鼠标、按键、滚轮 | 服务端 `hyprctl cursorpos` 跟随；按键不粘；`kk/pb/pw` 与浏览器同格式 | [wwclient.c](../native/wwclient.c) 的 `sdl_to_evdev` / `send_move` |
| NAT-09 | 输入租约隔离 | 浏览器与原生同时连 | 两边交替发输入 | 与 CONN-05 相同的 1.5 s 租约，不互相拽光标 | [native.rs](../gateway/src/native.rs) 的 `claim_input` |
| NAT-10 | peer 超时释放 | 客户端运行中 `kill -9` | 等 5 s 看网关日志 | 打印「原生客户端超时断开」，向 wwcap 发 `keysreset/mode reset/fps 0/astream 0` | [native.rs](../gateway/src/native.rs) 的 `release_and_idle` |
| NAT-11 | 空闲休眠联动 | 只有一个原生客户端 | 断开后等 30 s | wwcap 进入休眠；新客户端连上立即唤醒并出 IDR | [main.c](../capture/src/main.c) 的 `suspended` + `active` 分支 |
| NAT-12 | 弱网灵敏度矩阵 | `wwsrc --drop 0/0.1/1` | 各跑 10 s | 0%：60 fps/0 损帧；0.1%：仍 >45 fps 且 `undecodable=0`；1%：显著掉帧但不崩、不花屏 | [wwsrc.c](../native/wwsrc.c) 的 `--drop` |

---

## S. Sunshine-only 部署验收（2026-11-21 起，人工）

当前部署形态是 **Hyprland + Sunshine（Moonlight 客户端）**，浏览器链路已停用。下面这组用例
是这一形态的验收清单，全部为人工执行（`MAN-S*`）。

| 编号 | 用例 | 步骤 | 预期 |
|---|---|---|---|
| MAN-S1 | 冷启动链路 | `systemctl --user stop sunshine-wayland ww-session` → `systemctl --user start ww-session sunshine-wayland` | 30s 内两个单元 active；只有 1 个 Hyprland（无嵌套、非 safe-mode）；`HEADLESS-*` 按 `monitors.conf` 创建；47984/47989/47990 监听 |
| MAN-S2 | 抓帧后端就绪 | `scripts/sunshine-wayland.sh doctor` | 输出行有 `[wayland] Name: <输出>`、`Screencasting with Wayland's protocol`，失败帧 0，Hyprland EGL 错误 0，结论「环境正常」|
| MAN-S3 | 客户端串流 | Moonlight 连 `192.168.0.109` → `Desktop` | 约 1s 出画面；光标可见且跟随；键鼠可用（`hyprctl devices` 出现 `libvirtualhid-*`） |
| MAN-S4 | 纯 Wayland | `pgrep -a 'Xorg\|Xwayland'`；`systemctl --user show-environment \| grep -i display` | 两条都为空（`xwayland.enabled=false`、`DISPLAY` 已 unset） |
| MAN-S5 | X11 自启已关 | `systemctl is-enabled sddm x11-common`；`systemctl --user list-unit-files \| grep lxqt` | 前者 masked；后者 `app-lxqt-*@autostart` 全部 masked |
| MAN-S6 | 分辨率切换 | `scripts/sunshine-wayland.sh mode 2560x1440 1`（再切回 `1920x1080 1`） | 新 `HEADLESS-N` 直接是 2560x1440；旧输出被删；Sunshine 自动重启；`monitors.conf` 名字同步 |
| MAN-S7 | 独显守卫 | `scripts/switch-display.sh nvidia 3840x2160 2` → `scripts/sunshine-wayland.sh status` | Sunshine **拒绝启动**（未修复 bug，见 `docs/bugs/sunshine-nvidia-black-screen.md`），`doctor` 给出原因；切回 `intel 1920x1080 1` 恢复 |
| MAN-S8 | 浏览器链路已停 | `systemctl --user is-enabled ww-cap ww-gateway`；`pgrep -a 'wwcap\|wwgw'` | disabled；无进程 |
| MAN-S9 | 字号档位生效 | `scripts/font-scale.sh status` | GTK=1.3；waybar 16px/高 46；fuzzel 14pt；foot 18px；光标 31px；输出 scale=1.0 |
| MAN-S10 | 画面未缩放（不糊） | `grim -o HEADLESS-2 /tmp/s.png`，1:1 查看；`scripts/font-scale.sh status` 第 7 行 | PNG 恰为 1920×1080；文字笔画 1px 级过渡（无 2×2 重复像素块）；无 `GDK_SCALE`/`QT_SCALE_FACTOR` |
| MAN-S11 | 档位可回滚 | `scripts/font-scale.sh 1.0` → 再 `1.3` | 回滚后字号回到原始值（waybar 12px/38）；再切回 1.3 与 MAN-S9 一致 |

---

## 维护约定

- 新增自动化用例时：先在本文档对应分类下登记编号与预期，再在脚本中实现同名 `check(...)`，
  并在脚本头部注释中补充到本文档的锚点链接。
- 用例失败优先看脚本输出的 `detail` 字段，再对照本表的「对应位置」定位实现代码。
- 环境相关的临时排查脚本不属于用例，不要登记到本文档。
