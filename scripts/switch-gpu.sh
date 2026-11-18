#!/usr/bin/env bash
# 切换串流桌面所在的 GPU 路径：
#   intel  = 集显 i915（eDP-1 物理屏，h264_vaapi / VDEnc）
#   nvidia = 独显 GP107（headless 输出，h264_nvenc）
# 两条路径互斥：seatd 一个 seat 同时只允许一个活动会话。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GPU="${1:-intel}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

log() { printf '\033[1;34m[switch]\033[0m %s\n' "$*"; }

case "$GPU" in
    intel)
        CARD=/dev/dri/card1; ENC=vaapi; MAX=3200x1800 ;;
    nvidia)
        CARD=/dev/dri/card0; ENC=nvenc; MAX=3840x2160 ;;
    *)
        echo "用法: $0 {intel|nvidia}" >&2; exit 1 ;;
esac

log "停止现有会话"
pkill -f 'wwcap --video' 2>/dev/null || true
pkill -f start-hyprland 2>/dev/null || true
pkill -f 'Hyprland --config|/usr/bin/Hyprland' 2>/dev/null || true
sleep 3

log "在 $CARD 上启动 Hyprland"
(setsid env XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" LIBSEAT_BACKEND=seatd \
    AQ_DRM_DEVICES="$CARD" HYPRLAND_NO_SD_NOTIFY=1 start-hyprland \
    > "/tmp/hypr-$GPU.log" 2>&1 &)
sleep 13

HIS="$(ls -dt "$XDG_RUNTIME_DIR"/hypr/*/ 2>/dev/null | head -1 | xargs -r -I{} basename {})"
if [ -z "$HIS" ]; then
    echo "Hyprland 未启动，见 /tmp/hypr-$GPU.log" >&2
    exit 1
fi
export HYPRLAND_INSTANCE_SIGNATURE="$HIS"

if [ "$GPU" = nvidia ]; then
    log "创建 headless 输出（独显的 HDMI 未接）"
    hyprctl output create headless >/dev/null 2>&1 || true
    sleep 1
    OUT="$(hyprctl -j monitors 2>/dev/null | python3 -c \
        "import json,sys;print(json.load(sys.stdin)[0]['name'])" 2>/dev/null || echo HEADLESS-1)"
    hyprctl keyword monitor "$OUT,3840x2160@60,0x0,1" >/dev/null 2>&1 || true
else
    OUT="eDP-1"
    hyprctl keyword monitor "eDP-1,1920x1080@60,0x0,1" >/dev/null 2>&1 || true
fi

WL="$(ls "$XDG_RUNTIME_DIR" | grep -E '^wayland-[0-9]+$' | head -1)"
log "启动抓帧编码（$ENC，输出 $OUT）"
(setsid env XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" WAYLAND_DISPLAY="$WL" \
    "$ROOT/capture/build/wwcap" \
    --video "$XDG_RUNTIME_DIR/ww-video.sock" \
    --ctrl "$XDG_RUNTIME_DIR/wwcap-ctrl.sock" \
    --enc "$ENC" --output "$OUT" --max "$MAX" \
    > /tmp/wwcap.log 2>&1 &)
sleep 4

log "就绪：GPU=$GPU 输出=$OUT 编码=$ENC"
grep -aE '就绪|输出 ' /tmp/wwcap.log | tail -3 || true
