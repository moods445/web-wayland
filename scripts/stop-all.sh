#!/usr/bin/env bash
# 停止串流链路（--with-desktop 连桌面一起停）
set -uo pipefail

# systemd 管理时优先 systemctl：直接 pkill 会被 Restart=always 立刻拉回来
UNITS="ww-cap.service ww-gateway.service"
[ "${1:-}" = "--with-desktop" ] && UNITS="$UNITS ww-session.service"

if systemctl --user is-active --quiet ww-cap.service 2>/dev/null; then
    systemctl --user stop $UNITS
    echo "已停止 systemd 单元：$UNITS"
    exit 0
fi

pkill -f 'wwcap --video' 2>/dev/null && echo "已停止 wwcap"
pkill -f wwgw 2>/dev/null && echo "已停止 wwgw"

if [ "${1:-}" = "--with-desktop" ]; then
    pkill -f 'start-hyprland' 2>/dev/null
    pkill -f Hyprland 2>/dev/null
    echo "已停止 Hyprland 桌面"
fi
