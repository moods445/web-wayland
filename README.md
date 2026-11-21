# web-wayland

本机是一台 **Moonlight + Sunshine 远程桌面主机**：Sunshine 作服务端抓帧编码，Moonlight 作客户端连接；桌面是 Hyprland/Wayland 会话，只存在于 headless 虚拟输出上（物理面板 eDP-1 关闭、黑屏，只能通过 Moonlight 看）。

> **当前形态（2026-11-21 起）**：只保留 Moonlight + Sunshine 一条串流路径。
> 早期的自建浏览器串流链路（`wwcap` / `wwgw` / `web` / `native`）已整体移除，
> 仓库现在只包含桌面会话、Sunshine 适配、显示/GPU/字号运维脚本和 `doctor` 自检。
> 完整部署、排障与原理见 [docs/sunshine-moonlight.md](docs/sunshine-moonlight.md)。

## 目录结构

| 路径 | 用途 |
|---|---|
| `scripts/sunshine-wayland.sh` | Sunshine 起停、自检、诊断（`start` / `run` / `stop` / `status` / `doctor` / `mode`）|
| `scripts/install-sunshine-host.sh` | 一键切到「只做 Moonlight/Sunshine 主机」：mask 发行版单元、安装并启用两个用户单元 |
| `scripts/ww-session.service` | Hyprland 桌面会话单元（Sunshine 的抓帧目标）|
| `scripts/sunshine-wayland.service` | Sunshine 用户单元（`PartOf=ww-session`，会话重建跟着重建）|
| `scripts/switch-display.sh` | 统一切 GPU（集显/独显）与虚拟输出分辨率/缩放 |
| `scripts/switch-gpu.sh` | 重建 Hyprland 会话到指定 GPU（被 `switch-display.sh` 调用）|
| `scripts/font-scale.sh` | 1080p@1x 下的字号档位（1.0/1.15/1.3/1.45）|
| `scripts/deploy-config.sh` | 部署 `config/` 下的桌面配置与壁纸 |
| `scripts/pipewire/50-ww-desk.conf` | 虚拟音频 sink `ww-desk`（隔离麦克风，避免声学回环）|
| `config/` | Hyprland、waybar、foot、fuzzel、GTK 等桌面配置 |
| `assets/` | 壁纸等静态资源 |
| `docs/sunshine-moonlight.md` | **部署 / 运维 / 排障主文档** |
| `docs/bugs/sunshine-nvidia-black-screen.md` | 唯一遗留缺陷（独显虚拟输出抓帧黑屏）|
| `proto/` | 遗留的 Wayland 协议定义与传输头，取舍见 `docs/PROJECT-SUMMARY.md` |

## 快速开始

### 1. 依赖（Debian 13）

```bash
sudo apt install -y hyprland seatd waybar fuzzel swaybg foot grim \
    python3 pipewire nwg-displays
# Sunshine（本机为 Debian trixie 包）
# 用户需在 input 组，否则键鼠注入不生效：
sudo usermod -aG input "$USER"
```

### 2. 一键部署

```bash
scripts/install-sunshine-host.sh                   # 安装并启用用户单元，不动正在跑的会话
scripts/install-sunshine-host.sh --restart-session # 连 Hyprland 一起重启（窗口会关掉）
```

脚本会 mask 发行版自带的 Sunshine 单元（它在 X11 环境里启动会导致黑屏）、关闭 X11 相关自启，并安装启用两个用户单元：`ww-session`（Hyprland 桌面）+ `sunshine-wayland`（Sunshine）。开机由 `loginctl enable-linger` 保证无需登录即可拉起。

### 3. 客户端连接

1. Moonlight 里「添加主机」，填本机局域网 IP（本机 `192.168.0.109`；`avahi-daemon` 未运行，mDNS 发现不可用，按 IP 手动加）。
2. 浏览器打开 `https://<IP>:47990`（自签证书需手动信任），用 Moonlight 显示的四位 PIN 完成配对。
3. 选择 `Desktop` 开始串流。

监听端口：TCP `47984`（HTTPS/配对）、`47989`（HTTP）、`47990`（Web UI）、`48010`（RTSP），UDP `47998–48010`（串流期间建立）。

## 日常运维

```bash
systemctl --user status ww-session sunshine-wayland
scripts/sunshine-wayland.sh status                 # 汇总状态
scripts/sunshine-wayland.sh doctor                 # 黑屏时先跑这个：会话/抓帧卡/编码器/错误计数一次看完
journalctl --user -u sunshine-wayland -f           # 服务端日志
```

### 切 GPU / 分辨率

```bash
scripts/switch-display.sh                          # 查看当前档位
scripts/switch-display.sh intel 1920x1080 1        # 集显 + 1080p（当前默认档位）
scripts/switch-display.sh res 2560x1440 1          # 只改分辨率，不重启会话
scripts/sunshine-wayland.sh mode 3840x2160 2       # 直接改虚拟输出档位
```

当前默认 **1920×1080@60、scale=1**。虚拟输出的尺寸只能在创建时确定，改分辨率走 `mode` 子命令（新建→删旧→重启 Sunshine）。

### 字号

觉得字小**不要加 scale**（会发虚或砍半可用空间），只放大字号：

```bash
scripts/font-scale.sh 1.3      # 当前默认档
scripts/font-scale.sh status   # 自检：各程序字号 + 有无整屏缩放糊源
```

原理与档位对照见 [docs/sunshine-moonlight.md §12](docs/sunshine-moonlight.md#12-1080p1x-的字号放大不糊)。

## 已知限制

- **4K60 在本机不可得**：集显合成 4K 只有 ~20fps；独显能合成 4K 但 Sunshine 抓不到虚拟输出（`wlr` 的 dmabuf 导出失败、`kms` 不认 `HEADLESS` 连接器）。要 4K60 需给独显 HDMI 口插 EDID 诱骗器。详见 [独显黑屏 bug](docs/bugs/sunshine-nvidia-black-screen.md)。
- **画质比自建链路低一档**：Sunshine 的 VAAPI 路径只有 Rec.601 / 4:2:0，桌面文字锐度不如自建链路的 BT.709 / 4:4:4。
- **纯 Wayland**：X11 已全面关闭，依赖 X11 的程序（X11 版 Chrome、xdotool 等）不可用。
- **物理屏被禁用**：桌面只在 headless 输出上，`grim` 截图或 Moonlight 才能看到。

完整限制与故障排查见 [docs/sunshine-moonlight.md](docs/sunshine-moonlight.md)。
