#!/usr/bin/env bash
# 停止串流链路（保留 Hyprland 会话可选）
set -uo pipefail

pkill -f 'wwcap --video' 2>/dev/null && echo "已停止 wwcap"
pkill -f wwgw 2>/dev/null && echo "已停止 wwgw"

if [ "${1:-}" = "--with-desktop" ]; then
    pkill -f 'start-hyprland' 2>/dev/null
    pkill -f Hyprland 2>/dev/null
    echo "已停止 Hyprland 桌面"
fi
