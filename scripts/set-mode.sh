#!/usr/bin/env bash
# 切换串流分辨率并重启抓帧编码进程（编码器尺寸是启动时固定的）
# 用法: ./scripts/set-mode.sh 3840x2160@60 | 2560x1440@60 | 1920x1080@60
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="${1:-3840x2160@60}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

HIS="$(ls -dt "$XDG_RUNTIME_DIR"/hypr/*/ 2>/dev/null | head -1 | xargs -r -I{} basename {})"
export HYPRLAND_INSTANCE_SIGNATURE="$HIS"

hyprctl keyword monitor "eDP-1,$MODE,0x0,1" >/dev/null
sleep 1

pkill -f 'wwcap --video' 2>/dev/null || true
sleep 1

WL="$(ls "$XDG_RUNTIME_DIR" | grep -E '^wayland-[0-9]+$' | head -1)"
setsid env WAYLAND_DISPLAY="$WL" "$ROOT/capture/build/wwcap" \
    --video "$XDG_RUNTIME_DIR/ww-video.sock" \
    --ctrl "$XDG_RUNTIME_DIR/wwcap-ctrl.sock" \
    --audio "$XDG_RUNTIME_DIR/ww-audio.sock" \
    > /tmp/wwcap.log 2>&1 &

sleep 3
case "$MODE" in
    3840*) echo "已切到 $MODE（实测约 17fps，清晰优先）" ;;
    2560*) echo "已切到 $MODE（实测约 30fps，均衡）" ;;
    1920*) echo "已切到 $MODE（实测约 55fps，流畅优先）" ;;
    *)     echo "已切到 $MODE" ;;
esac
