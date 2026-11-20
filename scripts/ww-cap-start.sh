#!/usr/bin/env bash
# systemd 用的抓帧启动器：等 Hyprland socket → 复用/新建 headless 输出 → exec wwcap
# 开机时 ww-session 先起，Wayland socket 可能要几秒才出现，所以这里做等待和准备。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

# 1) 等 Wayland socket（最多 30 秒）
WL_DISPLAY=""
for _ in $(seq 1 60); do
    WL_DISPLAY="$(ls "$XDG_RUNTIME_DIR" 2>/dev/null | grep -E '^wayland-[0-9]+$' | head -1 || true)"
    [ -n "$WL_DISPLAY" ] && break
    sleep 0.5
done
[ -n "$WL_DISPLAY" ] || { echo "[cap] 30 秒内没等到 Wayland socket" >&2; exit 1; }
export WAYLAND_DISPLAY="$WL_DISPLAY"

# 2) Hyprland 实例签名：hyprctl 靠它找到运行中的 compositor
HIS="$(ls -dt "$XDG_RUNTIME_DIR"/hypr/*/ 2>/dev/null | head -1 | xargs -r basename)"
[ -n "$HIS" ] || { echo "[cap] 找不到 Hyprland 实例目录" >&2; exit 1; }
export HYPRLAND_INSTANCE_SIGNATURE="$HIS"

# 3) 1080p 抓帧要单独的 headless 输出（eDP-1 只支持 4K）；已存在就复用
pick_headless() {
    hyprctl -j monitors 2>/dev/null | python3 -c '
import json, sys
ms = json.load(sys.stdin)
print(next((m["name"] for m in ms if m["name"].startswith("HEADLESS")), ""))' 2>/dev/null || true
}
OUT="$(pick_headless)"
if [ -z "$OUT" ]; then
    hyprctl output create headless >/dev/null 2>&1 || true
    sleep 1
    OUT="$(pick_headless)"
fi
[ -n "$OUT" ] || { echo "[cap] 无法创建 headless 输出" >&2; exit 1; }
hyprctl keyword monitor "$OUT,1920x1080@60,0x0,1" >/dev/null 2>&1 || true

# 零拷贝抓帧必须和 Hyprland 用同一张卡：集显写 card1，独显写 card0
GBM_ARG=()
[ -n "${WW_GBM:-}" ] && GBM_ARG=(--gbm "$WW_GBM")

exec "$ROOT/capture/build/wwcap" \
    --video "$XDG_RUNTIME_DIR/ww-video.sock" \
    --ctrl "$XDG_RUNTIME_DIR/wwcap-ctrl.sock" \
    --audio "$XDG_RUNTIME_DIR/ww-audio.sock" \
    --capture "${WW_CAPTURE:-shm}" \
    --enc "${WW_ENC:-vaapi}" \
    --output "$OUT" \
    --max "${WW_MAX:-3200x1800}" \
    --fps "${WW_FPS:-60}" \
    --qp "${WW_QP:-20}" \
    --bitrate "${WW_BITRATE:-0}" \
    "${GBM_ARG[@]}"
