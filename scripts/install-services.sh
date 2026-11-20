#!/usr/bin/env bash
# 安装并启用开机自启：systemd 用户单元 + linger（无需图形登录）
#
# 单元关系：ww-session（Hyprland）→ ww-gateway（:8443）→ ww-cap（抓帧编码）
# 无会话时 wwcap 空闲 30 秒自动休眠：停抓帧、释放 GPU 编码器，进程常驻等唤醒。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"

mkdir -p "$UNIT_DIR"
for u in ww-session.service ww-gateway.service ww-cap.service; do
    install -m 644 "$ROOT/scripts/$u" "$UNIT_DIR/$u"
    echo "已安装 $UNIT_DIR/$u"
done

systemctl --user daemon-reload
# linger：开机后即使没人登录图形界面，这些用户单元也会被拉起
loginctl enable-linger "$USER"

systemctl --user enable ww-session.service ww-gateway.service ww-cap.service

# 已经在 systemd 管理下就只更新单元文件；run-all.sh 手动拉起过则提示先切换，
# 避免出现两个 Hyprland / 两份编码器
if systemctl --user is-active --quiet ww-cap.service; then
    echo "服务已在运行（systemd），仅更新了单元文件"
elif pgrep -f 'wwcap --video' >/dev/null || pgrep -f 'gateway/target/release/wwgw' >/dev/null \
    || pgrep -f 'start-hyprland' >/dev/null; then
    echo "检测到 run-all.sh 启动的进程，本次不启动服务（开机仍会自启）"
    echo "切换到 systemd 管理：scripts/stop-all.sh --with-desktop && systemctl --user start ww-session ww-gateway ww-cap"
else
    systemctl --user start ww-session.service ww-gateway.service ww-cap.service
    echo "已启动 ww-session / ww-gateway / ww-cap"
fi

echo "完成：开机自启已启用；无会话 30 秒后 wwcap 休眠（日志见 journalctl --user -u ww-cap -f）"
