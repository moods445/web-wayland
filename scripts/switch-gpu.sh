#!/usr/bin/env bash
# 切换串流桌面所在的 GPU 路径：
#   intel  = 集显 i915（物理屏 eDP-1 保持 4K，串流抓 headless 输出，h264_vaapi / VDEnc）
#   nvidia = 独显 GP107（headless 输出，h264_nvenc）
# 两条路径互斥：seatd 一个 seat 同时只允许一个活动会话。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GPU="${1:-intel}"

# 某些 IDE 终端会把自己的旧 libstdc++/libselinux 注入 LD_LIBRARY_PATH，
# 导致 start-hyprland 报 "GLIBCXX_3.4.31 not found"。启动前剔掉这些私有路径。
clean_ld_library_path() {
    local cleaned="" p
    local IFS=:
    for p in ${LD_LIBRARY_PATH-}; do
        case "$p" in
            */.trae-cn-server/*|*/.trae-cn/*) continue ;;
        esac
        cleaned="${cleaned:+$cleaned:}$p"
    done
    if [ -n "$cleaned" ]; then
        export LD_LIBRARY_PATH="$cleaned"
    else
        unset LD_LIBRARY_PATH
    fi
}
clean_ld_library_path

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

log() { printf '\033[1;34m[switch]\033[0m %s\n' "$*"; }

case "$GPU" in
    intel)
        CARD=/dev/dri/card1; ENC=vaapi; MAX=3200x1800; CAP=shm; MODE="${MODE:-1920x1080}" ;;
    nvidia)
        CARD=/dev/dri/card0; ENC=nvenc; MAX=3840x2160; CAP=sc; MODE="${MODE:-3840x2160}" ;;
    *)
        echo "用法: $0 {intel|nvidia}" >&2; exit 1 ;;
esac
# 虚拟输出的缩放（1 = 不缩放，2 = 4K 逻辑 1080p）
SCALE="${SCALE:-1}"
# 记录当前会话用的卡，sunshine-wayland.sh 据此选编码器（独显必须 nvenc）
mkdir -p "$HOME/.config/hypr"; echo "$GPU" > "$HOME/.config/hypr/display.state"

# systemd 常驻时不能只 pkill：`ww-session` 是 Restart=always，会立刻拉起一个新的
# Hyprland，而这个新实例拿不到 DRM master，会退化成**嵌套**会话（多出一个
# WAYLAND-1 伪输出）。后果是每个画面被合成两遍（fps 58→30、输入延迟翻倍），
# 且输入坐标系变成两个输出的并集，鼠标移动只有一半位移——表现为「鼠标非常卡」。
# Sunshine 依赖这个会话（PartOf=ww-session），必须一起停，否则它会挂在旧 socket 上。
if systemctl --user is-active --quiet ww-session 2>/dev/null \
    || systemctl --user is-active --quiet sunshine-wayland 2>/dev/null; then
    log "检测到 systemd 常驻，先停整条服务链（避免双 Hyprland 嵌套）"
    systemctl --user stop sunshine-wayland ww-cap ww-gateway ww-session 2>/dev/null || true
    sleep 3
fi

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
else
    log "创建 headless 输出（eDP-1 面板只有 4K 模式，不能当 1080p 抓帧目标）"
fi
hyprctl output create headless >/dev/null 2>&1 || true
sleep 1
OUT="$(hyprctl -j monitors 2>/dev/null | python3 -c \
    "import json,sys
ms=json.load(sys.stdin)
print(next((m['name'] for m in ms if m['name'].startswith('HEADLESS')), ms[0]['name']))" \
    2>/dev/null || echo HEADLESS-1)"
# 若还有别的输出在用，把 headless 放到它右侧（同坐标重叠会让 screencopy 抓到错层）；
# 只剩虚拟输出时用 0x0，与 nwg-displays 导出的 monitors.conf 保持一致
OX="$(hyprctl -j monitors 2>/dev/null | python3 -c "
import json,sys
ms=[m for m in json.load(sys.stdin) if m['name']!='$OUT' and m['width']>0]
print(max([m['x']+m['width'] for m in ms], default=0))" 2>/dev/null || echo 0)"
hyprctl keyword monitor "$OUT,$MODE@60,${OX:-0}x0,$SCALE" >/dev/null 2>&1 || true

if [ "${WITH_WWCAP:-0}" != "1" ]; then
    # 浏览器链路已停用（2026-11-21）：默认只起桌面，抓帧交给 Sunshine。
    # 真的要回到自建链路时加 WITH_WWCAP=1（或直接跑 scripts/install-services.sh）。
    log "不拉起 wwcap（已停用，需显式 WITH_WWCAP=1；输出留给 Sunshine）"
else
    WL="$(ls "$XDG_RUNTIME_DIR" | grep -E '^wayland-[0-9]+$' | head -1)"
    log "启动抓帧编码（$ENC，抓帧 $CAP，输出 $OUT）"
    (setsid env XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" WAYLAND_DISPLAY="$WL" \
        "$ROOT/capture/build/wwcap" \
        --video "$XDG_RUNTIME_DIR/ww-video.sock" \
        --ctrl "$XDG_RUNTIME_DIR/wwcap-ctrl.sock" \
        --audio "$XDG_RUNTIME_DIR/ww-audio.sock" \
        --capture "$CAP" --enc "$ENC" --output "$OUT" --max "$MAX" \
        > /tmp/wwcap.log 2>&1 &)
    sleep 4
    grep -aE '就绪|输出 ' /tmp/wwcap.log | tail -3 || true
fi

log "就绪：GPU=$GPU 输出=$OUT 分辨率=$MODE@60 scale=$SCALE 编码=$ENC 抓帧=$CAP"
