#!/usr/bin/env bash
# 一键拉起：Hyprland 会话 → wwgw 网关 → wwcap 抓帧编码
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
VIDEO="$XDG_RUNTIME_DIR/ww-video.sock"
CTRL="$XDG_RUNTIME_DIR/wwcap-ctrl.sock"

log() { printf '\033[1;34m[run]\033[0m %s\n' "$*"; }

# 1) Hyprland：固定用 iGPU(card1)，seatd 提供 DRM 访问
if ! pgrep -f 'Hyprland' >/dev/null; then
    log "启动 Hyprland（iGPU / eDP-1）"
    LIBSEAT_BACKEND=seatd AQ_DRM_DEVICES=/dev/dri/card1 \
        HYPRLAND_NO_SD_NOTIFY=1 setsid start-hyprland \
        > /tmp/ww-hyprland.log 2>&1 &
    sleep 6
fi

WL_DISPLAY="$(ls "$XDG_RUNTIME_DIR" 2>/dev/null | grep -E '^wayland-[0-9]+$' | head -1 || true)"
if [ -z "$WL_DISPLAY" ]; then
    echo "找不到 Wayland socket，检查 /tmp/ww-hyprland.log" >&2
    exit 1
fi
log "Wayland socket: $WL_DISPLAY"

# 2) 网关：HTTPS + WebTransport
if ! pgrep -f wwgw >/dev/null; then
    log "启动 wwgw（:8443 tcp+udp）"
    ( cd "$ROOT" && setsid "$ROOT/gateway/target/release/wwgw" > /tmp/wwgw.log 2>&1 & )
    sleep 2
fi

# 3) 抓帧编码
if ! pgrep -f 'wwcap --video' >/dev/null; then
    log "启动 wwcap"
    ( setsid env WAYLAND_DISPLAY="$WL_DISPLAY" "$ROOT/capture/build/wwcap" \
        --video "$VIDEO" --ctrl "$CTRL" > /tmp/wwcap.log 2>&1 & )
    sleep 2
fi

IP="$(ip -4 -o addr show scope global | awk '{print $4}' | cut -d/ -f1 | head -1)"
log "就绪 → https://$IP:8443/   直连参数：?auto=1  调试：?debug=1"
