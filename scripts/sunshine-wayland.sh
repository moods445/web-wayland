#!/usr/bin/env bash
# 旁挂 Sunshine（Moonlight 服务端）到现有的 headless 串流桌面。
#
# 为什么需要这个脚本：
#   1. Sunshine 的 wlr 抓帧在 **NVIDIA** 上会失败（自建 screencopy dmabuf 被合成器拒绝，
#      Hyprland 报 `eglCreateImageKHR ... EGL_BAD_PARAMETER`），在 **Intel i915** 上正常。
#      所以本脚本只支持 `switch-gpu.sh intel` 起出来的会话。
#   2. Sunshine 与 wwcap 不能同时抓帧（合成器每帧服务两次，实测帧率直接腰斩）。
#      本脚本会先停 wwcap。
#   3. 1080p60 是 Intel 的稳态档（4K 合成只有 20~22 fps，见 docs/streaming-conclusions.md）。
#
# 用法：
#   scripts/sunshine-wayland.sh start     # 检查环境 → 写配置 → 起 Sunshine
#   scripts/sunshine-wayland.sh stop
#   scripts/sunshine-wayland.sh status
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

# IDE 终端注入的私有 libstdc++ 会让系统二进制起不来
for p in ${LD_LIBRARY_PATH-}; do
    case "$p" in
        */.trae-cn-server/*|*/.trae-cn/*) unset LD_LIBRARY_PATH; break ;;
    esac
done

CONF="$HOME/.config/sunshine/sunshine.conf"
SLOG="$HOME/.config/sunshine/sunshine.log"
OUT_LOG=/tmp/sunshine-wayland.log
MODE=1920x1080@60

log() { printf '\033[1;34m[sun]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[sun]\033[0m %s\n' "$*" >&2; exit 1; }

wl_display() { ls "$XDG_RUNTIME_DIR" 2>/dev/null | grep -E '^wayland-[0-9]+$' | head -1; }

hypr_sig() {
    local d
    for d in $(ls -dt "$XDG_RUNTIME_DIR"/hypr/*/ 2>/dev/null); do
        if HYPRLAND_INSTANCE_SIGNATURE="$(basename "$d")" hyprctl -j monitors >/dev/null 2>&1; then
            basename "$d"; return 0
        fi
    done
    return 1
}

# 找（或建）headless 输出，并固定到 1080p60
ensure_output() {
    local sig out
    sig="$(hypr_sig)" || die "找不到运行中的 Hyprland，先执行 scripts/switch-gpu.sh intel"
    export HYPRLAND_INSTANCE_SIGNATURE="$sig"
    out="$(hyprctl -j monitors | python3 -c '
import json,sys
ms=json.load(sys.stdin)
print(next((m["name"] for m in ms if m["name"].startswith("HEADLESS")), ""))' 2>/dev/null || true)"
    if [ -z "$out" ]; then
        hyprctl output create headless >/dev/null 2>&1 || true
        sleep 1
        out="$(hyprctl -j monitors | python3 -c '
import json,sys
ms=json.load(sys.stdin)
print(next((m["name"] for m in ms if m["name"].startswith("HEADLESS")), ""))' 2>/dev/null || true)"
    fi
    [ -n "$out" ] || die "无法创建 headless 输出"
    hyprctl keyword monitor "$out,$MODE,0x0,1" >/dev/null 2>&1 || true
    echo "$out"
}

# Sunshine 读的是 ~/.config/sunshine/sunshine.conf，改前先备份；只动这三项
configure() {
    local out="$1"
    [ -f "$CONF" ] || die "没有 $CONF，先跑一次 sunshine 生成默认配置"
    mkdir -p "$HOME/.config/sunshine"
    local bak="$CONF.bak.$(date +%Y%m%d-%H%M%S)"
    cp -f "$CONF" "$bak"
    # 去掉旧的同名项与 X11 的 global_prep_cmd，再追加正确的
    python3 - "$CONF" "$out" <<'PY'
import re, sys
path, out = sys.argv[1], sys.argv[2]
lines = open(path).read().splitlines()
keep = []
for ln in lines:
    s = ln.strip()
    if re.match(r'^(capture|encoder|output_name)\s*=', s):
        continue
    if re.match(r'^#?\s*global_prep_cmd\s*=', s):
        continue          # X11 的 xrandr 预命令在 Wayland 下无意义
    keep.append(ln)
keep += [f'capture = wlr', f'encoder = vaapi', f'output_name = {out}']
open(path, 'w').write('\n'.join(keep) + '\n')
PY
    log "已写入 capture=wlr encoder=vaapi output_name=$out（备份 $bak）"
}

prereq() {
    command -v sunshine >/dev/null || die "未安装 sunshine"
    command -v hyprctl  >/dev/null || die "没有 hyprctl"
    [ -n "$(wl_display)" ] || die "没有 Wayland socket，先起 ww-session / switch-gpu.sh"
}

case "${1:-start}" in
start)
    prereq
    OUT="$(ensure_output)"
    # 双抓帧会把帧率打对折，必须让 wwcap 让位
    if pgrep -x wwcap >/dev/null; then
        log "检测到 wwcap 在抓帧，先停掉（避免双抓帧）"
        pkill -x wwcap || true
        sleep 1
    fi
    configure "$OUT"
    pkill -x sunshine 2>/dev/null || true
    sleep 1
    : > "$SLOG" 2>/dev/null || true
    log "启动 Sunshine（wlr 抓帧 → $OUT）"
    # setsid 必须留着：只加 nohup 时，启动它的终端/IDE 会话结束时 Sunshine 会一起被收走
    # （表现为「刚起来就连不上」，sunshine.log 停在各版本号之后没有退出原因）。
    #
    # /dev/uinput 是 root:input 0660，Sunshine 靠它注入键鼠。`usermod -aG input`
    # 之后要重新登录才生效，所以这里用 `sg input -c` 借组启动；已在组里就直接起。
    SENV="XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR WAYLAND_DISPLAY=$(wl_display) XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=Hyprland"
    if id -nG | tr ' ' '\n' | grep -qx input; then
        setsid env -u DISPLAY $SENV sunshine >>"$OUT_LOG" 2>&1 < /dev/null &
    else
        setsid sg input -c "env -u DISPLAY $SENV sunshine" >>"$OUT_LOG" 2>&1 < /dev/null &
    fi
    for _ in $(seq 1 20); do
        if grep -q "Screencasting with Wayland" "$SLOG" 2>/dev/null; then
            break
        fi
        sleep 0.5
    done
    grep -q "Screencasting with Wayland" "$SLOG" 2>/dev/null \
        && log "就绪：Web UI https://127.0.0.1:47990，串流端口 47984-47990" \
        || { log "未见 wlr 抓帧日志，检查 $SLOG"; tail -5 "$SLOG"; exit 1; }
    ;;
stop)
    pkill -x sunshine 2>/dev/null && log "Sunshine 已停止" || log "Sunshine 未在运行"
    ;;
status)
    if pgrep -x sunshine >/dev/null; then
        log "Sunshine 运行中（pid $(pgrep -x sunshine | tr '\n' ' ')）"
        grep -aE "Monitor [0-9]+ is|Screencasting|Creating encoder|Frame processing latency" "$SLOG" | tail -4 || true
    else
        log "Sunshine 未运行"
    fi
    pgrep -x wwcap >/dev/null && log "注意：wwcap 也在跑，会与 Sunshine 争抢抓帧" || true
    ;;
*)
    echo "用法: $0 {start|stop|status}" >&2; exit 1 ;;
esac
