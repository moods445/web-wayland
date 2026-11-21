# 未修复：独显会话 + 虚拟输出时 Sunshine 抓帧失败（Aquamarine / NVIDIA dmabuf 导入）

> **状态：未修复**（上游组合缺陷，本机无解，除非加硬件或改 Sunshine）。
> 本文是这条 bug 的**唯一正文**（随仓库入库）；本地 `bugs/` 目录被 `.gitignore` 忽略，里面只留一个指向本文的指针。
> 本文只保留这一条**仍然存在**的 bug；2026-11-21 修掉的其它黑屏原因（发行版单元用 X11 环境启动、
> 编码器跨 GPU、透明光标、输出名失配、X11 自启等）见
> [docs/sunshine-moonlight.md §11](../sunshine-moonlight.md#11-已修复的坑2026-11-21)。

## 1. 影响范围

- 只在 **Hyprland（Aquamarine）跑在 NVIDIA 独显上**、且**只使用 headless 虚拟输出**时触发；
- 集显（i915）会话不受影响（`capture=wlr` 正常，1080p60 可用）；
- 触发的组合：`capture = wlr` + 虚拟输出（`monitor = eDP-1, disable`，桌面只在 `HEADLESS-*` 上）。
  `capture = kms` 也救不了（见 §5）。

当前部署已经绕开它：`scripts/sunshine-wayland.sh` 检测到「独显 + wlr」会**直接拒绝启动**并打印原因
（`SUN_FORCE_NVIDIA=1` 可强行启动，但客户端一定是黑屏）。

## 2. 环境（复现时）

| 项 | 值 |
|---|---|
| 桌面 | Hyprland 0.55.2（Aquamarine 0.11），仅虚拟输出 `HEADLESS-2`，物理屏 `eDP-1` 已 `disable` |
| 显卡 | 独显 `/dev/dri/card0` = NVIDIA GTX 1050 Ti（驱动 580.178.04）；集显 `/dev/dri/card1` = i915 |
| 会话 | `AQ_DRM_DEVICES=/dev/dri/card0`（`scripts/switch-display.sh nvidia`） |
| 输出 | `HEADLESS-2` 3840x2160@60（也试过 scale=1） |
| Sunshine | Debian trixie 包 2026.914.233613，`capture=wlr`、`encoder=nvenc`、`output_name=HEADLESS-2`、debug 日志 |
| 客户端 | Mac 上的 Moonlight，配对正常，能建立会话并协商出 4K 码流 |
| 前提 | Sunshine **手工带正确 Wayland 环境**启动（`WAYLAND_DISPLAY` 有值、无 `DISPLAY`） |

## 3. 症状

- 客户端显示连接成功、有码率（约 63 Mbps）、分辨率协商正常，但**画面全黑**；
- 服务端日志每帧一条 `Error: [wayland] Frame capture failed`，并不断重建编码器。

关键日志（`~/.config/sunshine/sunshine.log`）：

```
Info: [wlgrab] Monitor 0 is HEADLESS-2:
Info: Screencasting with Wayland's protocol
Info: Creating encoder [hevc_nvenc]
Error: [wayland] Frame capture failed
```

## 4. 决定性证据（Hyprland 侧）

`$XDG_RUNTIME_DIR/hypr/<sig>/hyprland.log`：

```
ERR from aquamarine ]: [EGL] Command eglCreateImageKHR errored out with
EGL_BAD_PARAMETER (0x12300): In eglCreateImageKHR: requested buffer attributes are not supported
```

一次 4 分钟会话可累积 2280 条该错误，与 Sunshine 侧 228 条 `Frame capture failed` 一一对应。

用 `scripts/sunshine-wayland.sh doctor` 现在也能一眼看到：独显会话下
`捕获卡 = card0 (nvidia)` + `Hyprland EGL 导入错误 > 0` + `失败帧 > 0`。

## 5. 已排除的假设

| # | 假设 | 验证方法 | 结果 |
|---|---|---|---|
| 1 | 编码器用错（集显 vaapi 跨 GPU） | 分别用 `encoder=vaapi` / `encoder=nvenc` 启动 | **都不是**原因，两者都失败；nvenc 只是能让编码器创建成功 |
| 2 | Sunshine 选了集显的 EGL 设备 | debug 日志 + 串流期间读 `/proc/<pid>/fd` | 不是。`Debug: EGL: [NVIDIA]: version [1.5]`；nvenc 路径下不打开 `/dev/dri` 节点（走 Wayland EGL / CUDA-EGL 互操作） |
| 3 | 2x 缩放导致 buffer 属性不兼容 | 4K 改成 scale=1 再连 | 不是。scale=1 仍然每帧失败 |
| 4 | `output_name` 对不上，抓到空输出 | `[wlgrab] Monitor 0 is HEADLESS-2` / `[wayland] Name: HEADLESS-2` | 不是。枚举正常，输出确实被选中 |
| 5 | 桌面本身是黑的 | `grim -o HEADLESS-2` 截图看均值 | 不是。截到壁纸/waybar，均值 24.9/21.4/42.3 |
| 6 | Sunshine 没在跑 / 与 wwcap 抢抓帧 | `status`、`pgrep` | 不是。Sunshine 运行中，wwcap 已停 |
| 7 | dmabuf 抓帧在 nvidia 上整体不可用 | 同会话用 `wf-recorder -o HEADLESS-2 -c h264_nvenc` 录 4K | 不是。**录制成功，Hyprland EGL 错误数 0 增量**（说明合成器/驱动本身能服务 screencopy） |
| 8 | KMS 抓帧可绕过 | `capture=kms` + `setcap cap_sys_admin+p` | 不行。`Unknown Monitor connector type [HEADLESS]`，KMS 列表为空——独显侧唯一连接器 `HDMI-A-1` 未接线，虚拟输出不是真实 CRTC |

第 7 条是最有价值的结果：**同一个会话、同一张卡、同样 4K，另一个 screencopy 客户端可以正常出图**。
所以问题收窄为「Sunshine 通过 `zwp_linux_dmabuf_feedback_v1` 选出的 format/modifier 组合，
NVIDIA 的 `eglCreateImageKHR` 不接受」，而不是驱动或桌面整体坏了。

## 6. 根因分析

- 抓帧协议：`zwlr_screencopy_manager_v1`，客户端用 `zwp_linux_dmabuf_v1` 自行分配 buffer，
  合成器把画面渲染进去。合成器导入该 buffer 时
  `eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT, ...)` 返回
  `EGL_BAD_PARAMETER: requested buffer attributes are not supported`。
- Sunshine 二进制里可见 `eglQueryDmaBufFormatsEXT` / `eglQueryDmaBufModifiersEXT` /
  `zwp_linux_dmabuf_feedback_v1`，即它自己按 feedback 挑选格式与 modifier；
  它**没有 shm 回退**（二进制里有 `[wayland] SHM capture not implemented` 字符串），
  也没有可配置项强制格式/modifier。
- 结论：Sunshine（buffer 属性）与 Aquamarine/NVIDIA（导入能力）之间的组合缺陷；
  集显上不出问题，纯 nvidia 会话 + 虚拟输出上必现。

## 7. 为什么不修

- 改 Sunshine：要给 `capture=wlr` 加 shm 回退或强制 modifier，属于上游代码改动（本机只有二进制包）；
- 改 Aquamarine：属于系统库，NVIDIA 的 EGL 导入限制在闭源驱动里；
- 因此只能「绕」——见 §8。

## 8. 可选出路

| 方案 | 代价 | 预期 |
|---|---|---|
| **独显 HDMI 口插 EDID 诱骗器（推荐）** | 几十块硬件 | 造出真实 CRTC → `capture=kms` 可用，4K60；物理屏仍可关闭（`setcap cap_sys_admin+p /usr/bin/sunshine` 需要重新加） |
| 回集显（`scripts/switch-display.sh intel 1920x1080 1`）| 无 | **当前部署**：1080p60 稳定可用；4K 在集显上只有 ~20fps |
| 向上游报 issue / 试用其它版本 | 时间 | 附 §5、§6 证据；关注 Aquamarine 对 nvidia screencopy dmabuf 导入的修复 |
| 换抓帧实现 | 开发 | 给 Sunshine 加 shm 回退（工程量较大） |

## 9. 复现步骤

```bash
scripts/switch-display.sh nvidia 3840x2160 2   # 独显 + 4K（会话会重建）
SUN_FORCE_NVIDIA=1 scripts/sunshine-wayland.sh start   # 强行启动（脚本默认会拒绝）
# 客户端连 192.168.0.109 → 黑屏
scripts/sunshine-wayland.sh doctor                     # 应显示 driver=nvidia / 失败帧>0 / EGL 错误>0
grep -c 'Frame capture failed' ~/.config/sunshine/sunshine.log
grep -ci EGL_BAD_PARAMETER "$XDG_RUNTIME_DIR"/hypr/*/hyprland.log
```

## 10. 诊断命令（可复用）

```bash
# 1) Sunshine 侧失败帧数
grep -c 'Frame capture failed' ~/.config/sunshine/sunshine.log

# 2) Hyprland 侧 EGL 导入错误
grep -i EGL_BAD_PARAMETER "$XDG_RUNTIME_DIR"/hypr/*/hyprland.log | tail -3

# 3) Sunshine 的 EGL 厂商（确认没选错卡）
grep -a 'EGL: \[' ~/.config/sunshine/sunshine.log | tail -2

# 4) 一次性看全：会话/输出/dpms/抓帧卡/配置/进程环境/失败帧/EGL 错误/服务状态
scripts/sunshine-wayland.sh doctor

# 5) 黄金对照：同会话另一个 screencopy 客户端能不能抓
wf-recorder -o HEADLESS-2 -f /tmp/t.mp4 -c h264_nvenc -y   # 能录 = 桌面/驱动正常

# 6) KMS 是否可用
grep -a -A2 'Start of KMS monitor list' ~/.config/sunshine/sunshine.log
```

## 11. 相关文件

- [docs/sunshine-moonlight.md](../sunshine-moonlight.md)：部署形态、开机自启、已修复的坑（§11）、`doctor`
- [scripts/sunshine-wayland.sh](../../scripts/sunshine-wayland.sh)：独显守卫（退出码 3）、`doctor`
- [scripts/switch-display.sh](../../scripts/switch-display.sh)：GPU / 分辨率切换
- `~/.config/sunshine/sunshine.log`、`$XDG_RUNTIME_DIR/hypr/<sig>/hyprland.log`：两侧日志
