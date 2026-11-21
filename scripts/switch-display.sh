#!/usr/bin/env bash
# 统一切换串流桌面的 GPU 路径与虚拟输出分辨率/缩放。
#
# 用法：
#   scripts/switch-display.sh                        # 查看当前档位
#   scripts/switch-display.sh nvidia                 # 切独显（沿用当前分辨率）
#   scripts/switch-display.sh intel                  # 切集显（沿用当前分辨率）
#   scripts/switch-display.sh nvidia 3840x2160 2     # 独显 + 4K@2x
#   scripts/switch-display.sh intel 1920x1080 1      # 集显 + 1080p
#   scripts/switch-display.sh res 2560x1440 1        # 只改分辨率/缩放，不重启会话
#
# 档位会写入 ~/.config/hypr/monitors.conf，Hyprland 重启后依然生效。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MONCONF="$HOME/.config/hypr/monitors.conf"
STATE="$HOME/.config/hypr/display.state"

# IDE 终端会注入私有 libstdc++/libselinux，导致 Hyprland 起不来
clean_ld_library_path() {
    local cleaned="" p
    local IFS=:
    for p in ${LD_LIBRARY_PATH-}; do
        case "$p" in
            */.trae-cn-server/*|*/.trae-cn/*) continue ;;
        esac
        cleaned="${cleaned:+$cleaned:}$p"
    done
    if [ -n "$cleaned" ]; then export LD_LIBRARY_PATH="$cleaned"; else unset LD_LIBRARY_PATH; fi
}
clean_ld_library_path
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

log() { printf '\033[1;34m[display]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[display]\033[0m %s\n' "$*" >&2; exit 1; }
usage() { sed -n '2,13p' "${BASH_SOURCE[0]}" | sed -e 's/^# \{0,1\}//' -e '/^$/d'; }

live_sig() {
    local d
    for d in $(ls -dt "$XDG_RUNTIME_DIR"/hypr/*/ 2>/dev/null); do
        if HYPRLAND_INSTANCE_SIGNATURE="$(basename "$d")" hyprctl -j monitors >/dev/null 2>&1; then
            basename "$d"; return 0
        fi
    done
    return 1
}

# 优先取运行中的 HEADLESS 名，其次取配置里的，最后兜底 HEADLESS-2
out_name() {
    local sig name=""
    if sig="$(live_sig)"; then
        name="$(HYPRLAND_INSTANCE_SIGNATURE="$sig" hyprctl -j monitors 2>/dev/null | python3 -c "
import json,sys
print(next((m['name'] for m in json.load(sys.stdin) if m['name'].startswith('HEADLESS')), ''))" 2>/dev/null || true)"
    fi
    if [ -z "$name" ]; then
        name="$(grep -o 'HEADLESS-[0-9]\+' "$MONCONF" 2>/dev/null | head -1 || true)"
    fi
    echo "${name:-HEADLESS-2}"
}

# 打印 monitors.conf 里的 "分辨率 缩放"
read_conf() {
    python3 - "$MONCONF" <<'PY'
import re, sys
try:
    t = open(sys.argv[1]).read()
except OSError:
    print("1920x1080 1"); raise SystemExit
m = re.search(r'monitor\s*=\s*HEADLESS-\d+,\s*(\d+x\d+)(?:@\d+)?,\s*-?\d+x-?\d+,\s*([\d.]+)', t)
print(m.group(1), m.group(2)) if m else print("1920x1080 1")
PY
}

write_conf() {
    mkdir -p "$(dirname "$MONCONF")"
    cat > "$MONCONF" <<EOF
# 显示器配置（由 scripts/switch-display.sh 或 nwg-displays 写入，手改也可以）
# 格式：monitor = 名称, 分辨率@刷新率, XxY, 缩放
monitor = $1, $2@60, 0x0, $3
EOF
}

cur_gpu() { cat "$STATE" 2>/dev/null || echo intel; }

show() {
    local sig gpu="${1:-$(cur_gpu)}"
    log "GPU=$gpu  配置文件=$MONCONF"
    echo "  期望档位: $(read_conf | awk '{print $1"@60 scale="$2}')"
    if sig="$(live_sig)"; then
        echo "  实时输出:"
        HYPRLAND_INSTANCE_SIGNATURE="$sig" hyprctl -j monitors 2>/dev/null | python3 -c "
import json,sys
for m in json.load(sys.stdin):
    print('    %s  %dx%d@%.0f  scale=%s  logical=%.0fx%.0f  at %d,%d' % (m['name'],m['width'],m['height'],m['refreshRate'],m['scale'],m['width']/m['scale'],m['height']/m['scale'],m['x'],m['y']))"
    else
        echo "  （没有运行中的 Hyprland）"
    fi
    if pgrep -x sunshine >/dev/null 2>&1; then echo "  Sunshine: running"; else echo "  Sunshine: stopped"; fi
}

case "${1:-show}" in
    show|"")
        show
        ;;
    res)
        res="${2:-}"; scale="${3:-1}"
        [ -n "$res" ] || die "用法: $0 res <WxH> [scale]"
        # 虚拟输出的尺寸只能在创建时定（Aquamarine 的 headless backend 创建后只有
        # 当前这一档模式，`hyprctl keyword monitor` 改不动），交给 mode 子命令：
        # 新建目标尺寸的输出 → 删旧输出 → 重启 Sunshine。会话不用重启。
        "$ROOT/scripts/sunshine-wayland.sh" mode "$res" "$scale" \
            || die "切换失败，见上面输出"
        log "已切换 -> $res@60 scale=$scale"
        show
        ;;
    intel|nvidia)
        gpu="$1"
        res="${2:-}"; scale="${3:-}"
        if [ -z "$res" ]; then
            read -r res scale <<< "$(read_conf)"
        fi
        [ -n "${scale:-}" ] || scale=1
        out="$(out_name)"
        write_conf "$out" "$res" "$scale"
        mkdir -p "$(dirname "$STATE")"; echo "$gpu" > "$STATE"
        sun=0
        if pgrep -x sunshine >/dev/null 2>&1 \
            || systemctl --user is-enabled --quiet sunshine-wayland 2>/dev/null; then sun=1; fi
        log "切换 GPU=$gpu  档位=$res@60 scale=$scale"
        MODE="$res" SCALE="$scale" "$ROOT/scripts/switch-gpu.sh" "$gpu"
        # 会话重建后 headless 名字可能变（HEADLESS-N），把配置改成实际名字
        out="$(out_name)"
        write_conf "$out" "$res" "$scale"
        if [ "$gpu" = nvidia ]; then enc=nvenc; else enc=vaapi; fi
        if [ "$sun" = 1 ]; then
            # 会话是脚本手工拉起的（不是 systemd），Sunshine 用脚本起；有系统单元就用单元
            if systemctl --user is-enabled --quiet sunshine-wayland 2>/dev/null; then
                log "重新拉起 Sunshine（systemd 单元，encoder=$enc）"
                systemctl --user reset-failed sunshine-wayland 2>/dev/null || true
                systemctl --user start sunshine-wayland 2>/dev/null || \
                    log "Sunshine 未启动（独显会话上 wlr 抓帧不可用，见 scripts/sunshine-wayland.sh doctor）"
            else
                log "重新拉起 Sunshine（encoder=$enc）"
                SUN_ENCODER="$enc" "$ROOT/scripts/sunshine-wayland.sh" start \
                    || log "Sunshine 启动失败，见 ~/.config/sunshine/sunshine.log"
            fi
        fi
        show "$gpu"
        ;;
    *)
        usage; exit 1
        ;;
esac
