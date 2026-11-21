#!/usr/bin/env bash
# 物理屏 eDP-1 的桌面显隐控制（**只动 Hyprland 桌面输出，不碰文本控制台**）。
#
# 背景：本机桌面只存在于 headless 虚拟输出，物理面板 eDP-1 默认 `disable`、
# 保持黑屏（只能通过 Moonlight 看）。但 Hyprland 崩溃后 watchdog 用 safe-mode
# 拉起时加载自动生成的 stub 配置 recoverycfg.conf，不含 eDP-1 禁用规则 →
# 物理屏被点亮。本脚本提供 ensure-off 由 systemd 单元在会话启动后自动调用，
# 无论正常/安全模式都强制禁用 eDP-1，实现「永久禁止物理屏点亮桌面」。
#
# 用法：
#   scripts/physical-screen.sh status         # 查看 eDP-1/headless 当前状态
#   scripts/physical-screen.sh off            # 禁用物理屏桌面（保持黑屏，默认态）
#   scripts/physical-screen.sh on [分钟]      # 临时点亮物理屏桌面（默认 5 分钟内 timer 不关它）
#   scripts/physical-screen.sh ensure-off     # 确保 eDP-1 禁用且有 headless 输出（供自动调用）
#   scripts/physical-screen.sh ensure-off-wait # 同上，但先等会话就绪（供 systemd ExecStart 用）
#   scripts/physical-screen.sh --help
#
# 注意：文本控制台（getty/tty1-tty6、Ctrl+Alt+F2）由 systemd-logind 管，本脚本不碰；
#       「物理屏黑屏」只指 Hyprland 不往 eDP-1 渲染桌面。
set -uo pipefail

# on 命令写这个时间戳文件，hold 期内 ensure-off 跳过（timer 每 10s 会跑，不跳过就会立刻关掉）
HOLD_FILE="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/physical-screen.hold-until"

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

# IDE 终端注入的私有 libstdc++ 会让 hyprctl 起不来
for p in ${LD_LIBRARY_PATH-}; do
    case "$p" in
        */.trae-cn-server/*|*/.trae-cn/*) unset LD_LIBRARY_PATH; break ;;
    esac
done

PANEL="${WW_PANEL:-eDP-1}"
# eDP-1 原生 3840x2160，只有这一档 availableModes；用 preferred 才是稳定写法
# （写 1920x1080@60 会 modeset 后被 Hyprland 回退禁用 → 点了不亮）。
PANEL_MODE="${WW_PANEL_MODE:-preferred}"
log()  { printf '\033[1;34m[panel]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[panel]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[panel]\033[0m %s\n' "$*" >&2; exit 1; }

hypr_sig() {
    local d
    for d in $(ls -dt "$XDG_RUNTIME_DIR"/hypr/*/ 2>/dev/null); do
        if HYPRLAND_INSTANCE_SIGNATURE="$(basename "$d")" hyprctl -j monitors >/dev/null 2>&1; then
            basename "$d"; return 0
        fi
    done
    return 1
}

SIG=""
need_session() {
    SIG="$(hypr_sig)" || die "找不到运行中的 Hyprland（桌面会话没起来？systemctl --user status ww-session）"
}
hc() { HYPRLAND_INSTANCE_SIGNATURE="$SIG" hyprctl "$@" 2>/dev/null; }

# 输出一行：name<TAB>WxH<TAB>scale<TAB>x,y<TAB>enabled/disabled
# 注意：hyprctl -j monitors 不列出 disabled 的输出，所以 panel_state 为空 = 已禁用。
list_monitors() {
    hc -j monitors | python3 -c '
import json,sys
for m in json.load(sys.stdin):
    print("\t".join([m["name"], "%dx%d"%(m["width"],m["height"]), str(m["scale"]),
                     "%d,%d"%(m["x"],m["y"]), "disabled" if m.get("disabled") else "enabled"]))' 2>/dev/null
}
panel_enabled() { list_monitors | awk -F'\t' -v p="$PANEL" '$1==p && $5=="enabled"{print 1}'; }
headless_count() { list_monitors | awk -F'\t' '$1~/^HEADLESS/ && $5=="enabled"{c++} END{print c+0}'; }
# headless 右边缘 x（物理屏放到它右边，避免同坐标重叠让 screencopy 抓错层）
headless_right_x() {
    list_monitors | awk -F'\t' '$1~/^HEADLESS/ && $5=="enabled"{\
        split($2,w,"x"); split($4,xy,","); e=xy[1]+w[1]; if(e>m)m=e} END{print m+0}'
}

cmd_status() {
    need_session
    echo "会话: $SIG"
    echo "输出（name / 分辨率 / scale / 位置 / 状态）:"
    list_monitors | awk -F'\t' '{printf "  %-12s %-10s scale=%-4s at %-9s %s\n",$1,$2,$3,$4,$5}'
    if [ "$(panel_enabled)" = 1 ]; then
        echo "物理屏 $PANEL : enabled   ← 桌面正在点亮物理屏"
    else
        echo "物理屏 $PANEL : disabled  ← 桌面不点亮物理屏（默认态）"
    fi
    echo "headless 输出数: $(headless_count)  （期望 ≥1，桌面抓帧目标）"
}

# 真正禁用 PANEL。hyprctl keyword monitor <p>,disable 对「已经运行期点亮」的输出
# 返回 ok 却不生效（Hyprland 0.55 实测），必须用 reload 重新应用配置里的 disable 规则。
# 正常会话：正式配置的 monitor = eDP-1, disable 生效。
# safe-mode：加载的是 stub recoverycfg.conf（无 monitor 规则），需往 stub 追加一行
#           monitor = eDP-1, disable 再 reload。
disable_panel() {
    hc keyword monitor "$PANEL,disable" >/dev/null 2>&1 || true  # 先试 keyword（刚启动时够用）
    sleep 1
    [ "$(panel_enabled)" != 1 ] && return 0
    # keyword 没生效 → reload 应用配置里的 disable
    hc reload >/dev/null 2>&1 || true
    sleep 2
    [ "$(panel_enabled)" != 1 ] && return 0
    # safe-mode 下 reload 的是 stub（无 disable 规则）→ 给 stub 追加规则再 reload
    local stub
    for stub in "$XDG_RUNTIME_DIR"/hypr/*/recoverycfg.conf; do
        [ -f "$stub" ] || continue
        grep -q "^monitor[[:space:]]*=[[:space:]]*${PANEL}[[:space:]]*,[[:space:]]*disable" "$stub" \
            || printf '\nmonitor = %s, disable\n' "$PANEL" >> "$stub"
    done
    hc reload >/dev/null 2>&1 || true
    sleep 2
}

cmd_off() {
    need_session
    rm -f "$HOLD_FILE"          # 提前恢复：清掉 on 的持有期，timer 不会又把屏幕点亮
    if [ "$(panel_enabled)" != 1 ]; then
        log "$PANEL 已禁用，无需改动"
        return 0
    fi
    disable_panel
    if [ "$(panel_enabled)" = 1 ]; then
        die "$PANEL 仍启用：hyprctl keyword/reload 未能禁用，检查会话配置"
    fi
    log "$PANEL 已禁用（桌面不再渲染到物理屏；文本控制台不受影响）"
}

cmd_on() {
    need_session
    local x mins
    mins="${1:-5}"
    case "$mins" in ''|*[!0-9]*) die "分钟数应为正整数，如: $0 on 10" ;; esac
    [ "$mins" -ge 1 ] || die "分钟数应为正整数"
    x="$(headless_right_x)"
    [ -n "$x" ] || x=0
    hc keyword monitor "$PANEL,${PANEL_MODE},${x}x0,1" >/dev/null || die "hyprctl 下发失败"
    # 写持有期：timer 每 10s 跑 ensure-off，这期间跳过禁用，到点自动恢复
    printf '%s' "$(( $(date +%s) + mins * 60 ))" > "$HOLD_FILE"
    sleep 1
    log "$PANEL 已点亮（$PANEL_MODE，位于 headless 右侧 x=$x）"
    log "$mins 分钟后自动恢复禁用；提前恢复: scripts/physical-screen.sh off"
    warn "本地看桌面时 Moonlight 可能抓错层；看完可执行 off 立即恢复"
}

cmd_ensure_off() {
    need_session
    # on 命令的持有期内跳过（timer 每 10s 会跑一次，不跳过就会刚点亮又关掉）
    if [ -f "$HOLD_FILE" ]; then
        local until now
        until="$(cat "$HOLD_FILE" 2>/dev/null || echo 0)"
        now="$(date +%s)"
        if [ "$now" -lt "$until" ]; then
            return 0
        fi
        rm -f "$HOLD_FILE"
    fi
    # 会话启动后自动调用：无论正常还是 safe-mode，都保证 eDP-1 不参与桌面布局。
    # safe-mode 的 recoverycfg.conf 没有 monitor 规则，物理屏会被默认启用；
    # 这里用 hyprctl keyword 在运行期强制补上禁用，等效于正式配置里的那一行。
    if [ "$(headless_count)" -eq 0 ]; then
        # safe-mode 下可能连 headless 输出都没有（stub 不创建虚拟输出）。
        # 必须先建 headless 再禁物理屏，否则 Hyprland 会因「没有任何输出」直接退出。
        warn "没有 headless 输出，先创建虚拟输出（safe-mode 下 stub 不建）"
        hc output create headless >/dev/null 2>&1 || true
        for i in $(seq 1 10); do
            [ "$(headless_count)" -gt 0 ] && break
            sleep 0.5
        done
        [ "$(headless_count)" -gt 0 ] || warn "headless 输出创建失败，禁用物理屏后 Hyprland 可能退出"
    fi
    if [ "$(panel_enabled)" != 1 ]; then
        log "$PANEL 已禁用，无需改动"
        return 0
    fi
    disable_panel
    if [ "$(panel_enabled)" = 1 ]; then
        warn "$PANEL 仍启用：自动禁用未成功，物理屏可能被点亮（检查 hyprctl reload）"
        return 1
    fi
    log "$PANEL 已强制禁用（物理屏桌面保持黑屏；文本控制台不受影响）"
}

cmd_ensure_off_wait() {
    # systemd 在 ww-session 刚起时就调用，会话可能还没就绪：轮询最多 60s。
    local i
    for i in $(seq 1 120); do
        if SIG="$(hypr_sig)"; then
            cmd_ensure_off
            return 0
        fi
        sleep 0.5
    done
    warn "60s 内没有就绪的 Hyprland 会话，跳过 physical-screen ensure-off"
}

case "${1:-status}" in
    status)           cmd_status ;;
    off)              cmd_off ;;
    on)               cmd_on ;;
    ensure-off)       cmd_ensure_off ;;
    ensure-off-wait)  cmd_ensure_off_wait ;;
    -h|--help)        sed -n '2,22p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' ;;
    *) echo "用法: $0 {status|off|on|ensure-off|ensure-off-wait|--help}" >&2; exit 1 ;;
esac
