# web-wayland

本机是一台 **Moonlight + Sunshine 远程桌面主机**：Sunshine 服务端抓帧编码，Moonlight 客户端连接；
桌面是 Hyprland/Wayland，只跑在 headless 虚拟输出上（物理屏 eDP-1 关闭，只能通过 Moonlight 看）。

完整部署、排障与原理见 [docs/sunshine-moonlight.md](docs/sunshine-moonlight.md)。

## 部署

```bash
scripts/install-sunshine-host.sh                    # 安装/更新用户单元，不动当前会话
scripts/install-sunshine-host.sh --restart-session  # 连桌面一起重启（会关掉当前窗口）
```

## 客户端连接

1. Moonlight「添加主机」，填本机 IP（当前 `192.168.0.109`，mDNS 未启用）。
2. 浏览器打开 `https://<IP>:47990`（自签证书需信任），用 Moonlight 显示的 PIN 配对。
3. 选择 `Desktop` 开始串流。

## 日常使用

```bash
scripts/doctor.sh                    # 黑屏先跑：全量体检 + 每项解决步骤
scripts/doctor.sh --fix              # 体检后自动修复（重启/切 GPU 会标注）
systemctl --user status ww-session sunshine-wayland
journalctl --user -u sunshine-wayland -f
```

黑屏/串流问题的自检、safe-mode、物理屏控制见 [主文档 §8](docs/sunshine-moonlight.md#8-故障排查)
与 [§13](docs/sunshine-moonlight.md#13-物理屏-edp-1-与-safe-mode)；崩溃原因见 [§14](docs/sunshine-moonlight.md#14-hyprland-崩溃诊断2026-11-21)。

## 常用调整

```bash
scripts/switch-display.sh intel 1920x1080 1   # 切 GPU / 分辨率（当前默认集显 1080p）
scripts/font-scale.sh 1.3                     # 字小调字号（不要加 scale）
scripts/physical-screen.sh on                 # 临时点亮物理屏（默认 5 分钟，到点自动恢复黑屏）
```
