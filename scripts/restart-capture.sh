#!/usr/bin/env bash
# 客户端「重启流服务」入口（由 wwgw 收到 {"t":"restart"} 时调用，也可手工执行）
#
#   restart-capture.sh          只重启 wwcap：重建抓帧会话 + 编码器（桌面里的窗口都保留）
#   restart-capture.sh --deep   连 Hyprland 一起重启（窗口会关闭，最后手段）
#
# 背景：Hyprland 的 wlr-screencopy 一旦卡住，wwcap 会一直「0 fps」但进程还活着；
# 只杀 wwcap 不够（卡住的拷贝请求留在合成器里），所以这里先 forcerendererreload，
# 再按需重启整个桌面会话。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
LOG=/tmp/wwcap.log
DEEP=0
[ "${1:-}" = "--deep" ] && DEEP=1

log() { printf '[restart] %s\n' "$*"; }

# ── 1) 记录当前 wwcap 的启动参数，重启后原样复用 ──────────────────
# --output/--enc/--capture 都可能被 switch-gpu.sh 或分辨率协商改过，不能写死
ARGS=()
PID="$(pgrep -f 'capture/build/wwcap' | head -1 || true)"
if [ -n "$PID" ] && [ -r "/proc/$PID/cmdline" ]; then
    mapfile -d '' -t RAW < "/proc/$PID/cmdline" 2>/dev/null || true
    if [ "${#RAW[@]}" -gt 1 ]; then
        ARGS=("${RAW[@]:1}")
        log "沿用现有参数：${ARGS[*]}"
    fi
fi
if [ "${#ARGS[@]}" -eq 0 ]; then
    HIS="$(ls -dt "$XDG_RUNTIME_DIR"/hypr/*/ 2>/dev/null | head -1 | xargs -r basename)"
    OUT="HEADLESS-1"
    if [ -n "$HIS" ]; then
        export HYPRLAND_INSTANCE_SIGNATURE="$HIS"
        OUT="$(timeout 5 hyprctl -j monitors 2>/dev/null | python3 -c \
            'import json,sys
ms=json.load(sys.stdin)
print(next((m["name"] for m in ms if m["name"].startswith("HEADLESS")), ms[0]["name"]))' \
            2>/dev/null || echo HEADLESS-1)"
    fi
    ARGS=(--video "$XDG_RUNTIME_DIR/ww-video.sock" --ctrl "$XDG_RUNTIME_DIR/wwcap-ctrl.sock"
          --audio "$XDG_RUNTIME_DIR/ww-audio.sock" --capture shm --enc vaapi
          --output "$OUT" --max 3200x1800)
    log "没有在跑的 wwcap，使用默认参数：${ARGS[*]}"
fi

# ── 2) 先推一下卡住的 screencopy 队列（两条路径都需要）────────────
HIS="${HYPRLAND_INSTANCE_SIGNATURE:-$(ls -dt "$XDG_RUNTIME_DIR"/hypr/*/ 2>/dev/null | head -1 | xargs -r basename)}"
if [ -n "$HIS" ]; then
    export HYPRLAND_INSTANCE_SIGNATURE="$HIS"
    timeout 5 hyprctl dispatch forcerendererreload >/dev/null 2>&1 || true
fi

# ── 3) systemd 管理时交给 systemctl：直接 kill 会被 Restart=always 拉回来，
#       再手工起一份就会有两个 wwcap 抢同一对 socket ──────────────
if systemctl --user is-active --quiet ww-cap.service 2>/dev/null; then
    log "检测到 systemd 管理，改用 systemctl 重启"
    if [ "$DEEP" = 1 ]; then
        systemctl --user restart ww-session.service ww-cap.service
    else
        systemctl --user restart ww-cap.service
    fi
    sleep 3
    if systemctl --user is-active --quiet ww-cap.service; then
        log "wwcap 已重启（journalctl --user -u ww-cap -f）"
        exit 0
    fi
    log "ww-cap 重启失败：journalctl --user -u ww-cap -n 30"
    exit 1
fi

# ── 4) 停掉旧采集（TERM → 最多 3s → KILL）────────────────────────
if [ -n "$PID" ]; then
    kill -TERM "$PID" 2>/dev/null || true
    for _ in $(seq 1 30); do
        kill -0 "$PID" 2>/dev/null || break
        sleep 0.1
    done
    kill -KILL "$PID" 2>/dev/null || true
fi
pkill -f 'capture/build/wwcap' 2>/dev/null || true
sleep 0.5

# ── 3) 强制模式：重启 Hyprland 会话，交给 run-all 重新拉起 ─────────
if [ "$DEEP" = 1 ]; then
    log "强制模式：重启 Hyprland（桌面里的所有程序都会关闭）"
    pkill -f 'start-hyprland' 2>/dev/null || true
    pkill -f 'Hyprland' 2>/dev/null || true
    sleep 3
    # run-all 会跳过仍在运行的 wwgw，只重建 Hyprland / headless 输出 / wwcap
    setsid bash "$ROOT/scripts/run-all.sh" > /tmp/ww-restart.log 2>&1 &
    log "已通过 run-all 重新拉起（日志 /tmp/ww-restart.log）"
    exit 0
fi

# ── 4) 重建抓帧 ───────────────────────────────────────────────────
WL="$(ls "$XDG_RUNTIME_DIR" 2>/dev/null | grep -E '^wayland-[0-9]+$' | head -1)"
if [ -z "$WL" ]; then
    log "找不到 Wayland socket（Hyprland 没在跑？需要 --deep）"
    exit 2
fi

printf '\n===== %s 客户端请求重启 =====\n' "$(date '+%F %T')" >> "$LOG"
setsid env XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" WAYLAND_DISPLAY="$WL" \
    HYPRLAND_INSTANCE_SIGNATURE="$HIS" \
    "$ROOT/capture/build/wwcap" "${ARGS[@]}" >> "$LOG" 2>&1 &

# ── 5) 只校验「进程起来且进入运行态」，出帧由客户端按帧计数确认 ────
for _ in $(seq 1 40); do
    if pgrep -f 'capture/build/wwcap' >/dev/null && grep -q '\[cap\] 运行中' "$LOG"; then
        log "wwcap 已重启（日志 $LOG）"
        exit 0
    fi
    sleep 0.1
done

log "wwcap 启动失败，见 $LOG"
exit 1
