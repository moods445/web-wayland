#!/usr/bin/env bash
# 一键拉起：Hyprland 会话 → wwgw 网关 → wwcap 抓帧编码
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

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
VIDEO="$XDG_RUNTIME_DIR/ww-video.sock"
CTRL="$XDG_RUNTIME_DIR/wwcap-ctrl.sock"
AUDIO="$XDG_RUNTIME_DIR/ww-audio.sock"

log() { printf '\033[1;34m[run]\033[0m %s\n' "$*"; }

# 0) 音频：确保 ww-desk 虚拟 sink 存在并设为默认输出
#    硬件 sink 的 monitor 可能混入麦克风（ALC3246 默认 Loopback Mixing），
#    所以应用声音改走 null sink，wwcap 只抓它的 monitor（见 TEST-CASES K-08）
has_ww_desk() {
    pw-dump 2>/dev/null | python3 -c '
import json,sys
try: d=json.load(sys.stdin)
except Exception: sys.exit(1)
sys.exit(0 if any(((o.get("info") or {}).get("props") or {}).get("node.name")=="ww-desk" for o in d) else 1)'
}

ensure_audio_sink() {
    local conf_dir="$HOME/.config/pipewire/pipewire.conf.d" restart=0
    if ! cmp -s "$ROOT/scripts/pipewire/50-ww-desk.conf" "$conf_dir/50-ww-desk.conf" 2>/dev/null; then
        mkdir -p "$conf_dir"
        cp "$ROOT/scripts/pipewire/50-ww-desk.conf" "$conf_dir/"
        log "已安装 ww-desk 虚拟音频 sink 配置"
        restart=1
    fi
    has_ww_desk || restart=1
    if [ "$restart" = 1 ]; then
        log "重启 PipeWire 创建 ww-desk"
        systemctl --user restart pipewire.service pipewire-pulse.service wireplumber.service 2>/dev/null || true
        sleep 2
    fi
    has_ww_desk || { log "警告：ww-desk 未创建，音频将静音"; return; }
    local id
    id="$(pw-dump 2>/dev/null | python3 -c '
import json,sys
for o in json.load(sys.stdin):
    if ((o.get("info") or {}).get("props") or {}).get("node.name")=="ww-desk":
        print(o["id"]); break' 2>/dev/null || true)"
    [ -n "$id" ] && wpctl set-default "$id" >/dev/null 2>&1 || true
}
ensure_audio_sink

# 1) Hyprland：固定用 iGPU(card1)，seatd 提供 DRM 访问
if ! pgrep -f 'Hyprland' >/dev/null; then
    log "启动 Hyprland（iGPU）"
    # 必须显式指定配置：新版 Hyprland 默认优先加载 hyprland.lua，
    # 不指定会退回 /usr/share/hypr 的示例配置，绑定与显示器规则全部失效
    LIBSEAT_BACKEND=seatd AQ_DRM_DEVICES=/dev/dri/card1 \
        HYPRLAND_NO_SD_NOTIFY=1 setsid start-hyprland -- \
        --config "$HOME/.config/hypr/hyprland.conf" \
        > /tmp/ww-hyprland.log 2>&1 &
    sleep 6
fi

WL_DISPLAY="$(ls "$XDG_RUNTIME_DIR" 2>/dev/null | grep -E '^wayland-[0-9]+$' | head -1 || true)"
if [ -z "$WL_DISPLAY" ]; then
    echo "找不到 Wayland socket，检查 /tmp/ww-hyprland.log" >&2
    exit 1
fi
log "Wayland socket: $WL_DISPLAY"

# eDP-1 只支持 4K，1080p 抓帧要单独建一个 headless 输出
HIS="$(ls -dt "$XDG_RUNTIME_DIR"/hypr/*/ 2>/dev/null | head -1 | xargs -r basename)"
if [ -n "$HIS" ]; then
    export HYPRLAND_INSTANCE_SIGNATURE="$HIS"
    hyprctl output create headless >/dev/null 2>&1 || true
    sleep 1
    OUT="$(hyprctl -j monitors 2>/dev/null | python3 -c \
        "import json,sys
ms=json.load(sys.stdin)
print(next((m['name'] for m in ms if m['name'].startswith('HEADLESS')), ms[0]['name']))" \
        2>/dev/null || echo HEADLESS-1)"
    hyprctl keyword monitor "$OUT,1920x1080@60,0x0,1" >/dev/null 2>&1 || true
else
    OUT="HEADLESS-1"
fi

# 2) 网关：HTTPS + WebTransport
if ! pgrep -f wwgw >/dev/null; then
    log "启动 wwgw（:8443 tcp+udp）"
    ( cd "$ROOT" && setsid "$ROOT/gateway/target/release/wwgw" > /tmp/wwgw.log 2>&1 & )
    sleep 2
fi

# 3) 抓帧编码
if ! pgrep -f 'wwcap --video' >/dev/null; then
    log "启动 wwcap（shm/vaapi，输出 $OUT）"
    ( setsid env WAYLAND_DISPLAY="$WL_DISPLAY" "$ROOT/capture/build/wwcap" \
        --video "$VIDEO" --ctrl "$CTRL" --audio "$AUDIO" \
        --capture shm --enc vaapi --output "$OUT" --max 3200x1800 \
        > /tmp/wwcap.log 2>&1 & )
    sleep 2
fi

IP="$(ip -4 -o addr show scope global | awk '{print $4}' | cut -d/ -f1 | head -1)"
log "就绪 → https://$IP:8443/   直连参数：?auto=1  调试：?debug=1"
