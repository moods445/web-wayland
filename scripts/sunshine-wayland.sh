#!/usr/bin/env bash
# Moonlight + Sunshine 常驻服务端：起停、自检、诊断。
#
# 本机只保留 **Moonlight + Sunshine** 一条串流路径，桌面抓帧交给 Sunshine。
#
# 用法：
#   scripts/sunshine-wayland.sh start      # 手动起（探测环境 → 写配置 → 后台常驻）
#   scripts/sunshine-wayland.sh run        # 前台 exec，给 systemd 单元用
#   scripts/sunshine-wayland.sh stop|restart|status
#   scripts/sunshine-wayland.sh doctor     # 黑屏时先跑这个
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
OUT_LOG="${SUN_LOG:-/tmp/sunshine-wayland.log}"
SERVICE=sunshine-wayland.service
DISTRO_UNIT=app-dev.lizardbyte.app.Sunshine.service

log()  { printf '\033[1;34m[sun]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[sun]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[sun]\033[0m %s\n' "$*" >&2; exit 1; }

# ── 环境探测 ────────────────────────────────────────────────────────────

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

hyprctl_live() { HYPRLAND_INSTANCE_SIGNATURE="$1" hyprctl "${@:2}"; }

# 合成器实际渲染用的卡：问 zwp_linux_dmabuf_feedback 的 main device，
# 比读 ~/.config/hypr/display.state 可靠（后者只是脚本写的"上次选了哪张"，会过期）。
dmabuf_device() {
    command -v wayland-info >/dev/null || return 1
    wayland-info 2>/dev/null \
        | sed -n '/interface: .zwp_linux_dmabuf_v1/,/^interface:/p' \
        | sed -n 's/.*main device:.*\(\/dev\/dri\/[a-z]*[0-9]*\).*/\1/p' \
        | head -1
}

# 备用：从 Hyprland 日志里认（wayland-info 未安装时）
log_device() {
    local l
    l="$(ls -t "$XDG_RUNTIME_DIR"/hypr/*/hyprland.log 2>/dev/null | head -1)"
    [ -n "$l" ] || return 1
    sed -n 's/.*Starting backend for \(\/dev\/dri\/card[0-9]*\).*/\1/p' "$l" | tail -1
}

card_driver() {
    local node="/sys/class/drm/$(basename "$1")/device/driver"
    [ -e "$node" ] && basename "$(readlink -f "$node")" || echo unknown
}

encoder_for_driver() {
    case "$1" in
        nvidia*)               echo nvenc ;;
        i915|xe|amdgpu|radeon*) echo vaapi ;;
        *)                     echo vaapi ;;
    esac
}

# 只认 HEADLESS*（桌面只存在于虚拟输出）。**不要**回退到 eDP-1/HDMI-A-1 之类的
# 真实输出：安全模式（recoverycfg.conf）会把物理屏拉起来，一旦回退，Sunshine 抓到
# 的就是物理屏、monitors.conf 也会被改写成物理屏名字，越修越乱。
find_output() {
    hyprctl_live "$1" -j monitors 2>/dev/null | python3 -c '
import json,sys
ms=[m for m in json.load(sys.stdin) if not m.get("disabled") and m["name"].startswith("HEADLESS")]
print(ms[0]["name"] if ms else "")' 2>/dev/null || true
}

list_headless() {
    hyprctl_live "$1" -j monitors 2>/dev/null | python3 -c '
import json,sys
print("\n".join(m["name"] for m in json.load(sys.stdin) if m["name"].startswith("HEADLESS") and not m.get("disabled")))' 2>/dev/null || true
}

ensure_output() {
    local sig="$1" out i name res pos scale extra
    out="$(find_output "$sig")"
    if [ -z "$out" ]; then
        read -r name res pos scale <<< "$(conf_monitor)"
        # 虚拟输出的尺寸只能在**创建时**定：Aquamarine 的 headless backend 创建后
        # availableModes 只有当下那一档，`hyprctl keyword monitor` 改不动它。
        # 创建前先把候选名字（HEADLESS-N 会递增，实际叫几号事先不知道）的规则
        # 全注册一遍，创建出来的那个就会直接是目标尺寸。
        for i in $(seq 1 16); do
            hyprctl_live "$sig" keyword monitor "HEADLESS-$i,$res@60,${pos:-0x0},${scale:-1}" >/dev/null 2>&1 || true
        done
        hyprctl_live "$sig" output create headless >/dev/null 2>&1 || true
        for i in $(seq 1 10); do     # 输出创建是异步的，等它出现在 monitors 里
            sleep 0.5
            out="$(find_output "$sig")"
            [ -n "$out" ] && break
        done
    fi
    [ -n "$out" ] || die "无法创建 headless 输出（hyprctl output create headless 失败）"
    # 多出来的虚拟输出必须删掉：同坐标重叠时 screencopy 会抓到错的那一层（黑屏）
    for extra in $(list_headless "$sig" | grep -v "^${out}$" || true); do
        warn "删除多余的虚拟输出 $extra（多个 HEADLESS 同坐标会让抓帧黑屏）" >&2
        hyprctl_live "$sig" output remove "$extra" >/dev/null 2>&1 || true
    done
    # 本函数用 stdout 返回输出名，内部日志一律走 stderr，别污染 $( )
    sync_monitor "$sig" "$out" >&2
    echo "$out"
}

MONCONF="$HOME/.config/hypr/monitors.conf"

# monitors.conf 里的档位（名字 分辨率 位置 缩放）。会话重启后 headless 名字会变
# （HEADLESS-2 → HEADLESS-1/3），配置项就对不上了，这里按实际名字重放一次。
conf_monitor() {
    python3 - "$MONCONF" <<'PY'
import re, sys
try:
    t = open(sys.argv[1]).read()
except OSError:
    print("HEADLESS-1 1920x1080 0x0 1"); raise SystemExit
pat = re.compile(r'monitor\s*=\s*(\S+?),\s*(\d+x\d+)(?:@\d+)?,\s*(-?\d+x-?\d+),\s*([\d.]+)')
rules = [m.groups() for m in pat.finditer(t)]
hl = [r for r in rules if r[0].startswith("HEADLESS")]   # 优先取虚拟输出的档位
pick = (hl or rules or [("HEADLESS-1", "1920x1080", "0x0", "1")])[0]
print("%s %s %s %s" % pick)
PY
}

# 统一的 monitor 档位写法（带说明注释；改动后 monitors.conf 与仓库默认文件保持一致）
monitor_conf_write() {
    local name="$1" res="$2" pos="${3:-0x0}" scale="${4:-1}"
    mkdir -p "$(dirname "$MONCONF")"
    {
        printf '# 显示器配置（由 scripts/switch-display.sh 或 scripts/sunshine-wayland.sh mode 写入，手改也可以）\n'
        printf '# 格式：monitor = 名称, 分辨率@刷新率, XxY, 缩放\n'
        printf '#\n'
        printf '# 默认档位：1920x1080@60 + scale 1 → 逻辑 1920×1080，客户端 1:1 像素，最清晰。\n'
        printf '# 字小请调字号：scripts/font-scale.sh [1.0|1.15|1.3|1.45]，不要用 scale 2（会发虚）。\n'
        printf 'monitor = %s, %s@60, %s, %s\n' "$name" "$res" "$pos" "$scale"
    } > "$MONCONF"
}

sync_monitor() {
    local sig="$1" out="$2" name res pos scale
    case "$out" in HEADLESS*) ;; *) warn "输出 $out 不是虚拟输出，跳过档位同步"; return 0 ;; esac
    read -r name res pos scale <<< "$(conf_monitor)"
    hyprctl_live "$sig" keyword monitor "$out,$res@60,${pos:-0x0},${scale:-1}" >/dev/null 2>&1 || true
    if [ "$name" != "$out" ]; then
        log "输出名变化：$name → $out，更新 $MONCONF"
        monitor_conf_write "$out" "$res" "${pos:-0x0}" "${scale:-1}"
    fi
}

# ── 写配置 ──────────────────────────────────────────────────────────────

# 只改 capture / encoder / output_name；顺便清掉排查期留下的 debug 日志与 X11 预命令
write_conf() {
    local out="$1" enc="$2" cap="$3" lvl="${4:-info}"
    [ -f "$CONF" ] || die "没有 $CONF，先跑一次 sunshine 生成默认配置"
    if [ "${SUN_NO_WRITE:-0}" = 1 ]; then log "SUN_NO_WRITE=1，跳过写配置"; return 0; fi
    python3 - "$CONF" "$out" "$enc" "$cap" "$lvl" <<'PY'
import re, sys
path, out, enc, cap, lvl = sys.argv[1:6]
lines = open(path).read().splitlines()
keep = []
for ln in lines:
    s = ln.strip()
    if re.match(r'^(capture|encoder|output_name|min_log_level)\s*=', s):
        continue
    if re.match(r'^#?\s*global_prep_cmd\s*=', s):
        continue          # X11 的 xrandr 预命令在 Wayland 下无意义
    keep.append(ln)
keep += [f'capture = {cap}', f'encoder = {enc}', f'output_name = {out}', f'min_log_level = {lvl}']
new = '\n'.join(keep) + '\n'
old = open(path).read()
if new == old:
    print('unchanged'); raise SystemExit
open(path + '.bak', 'w').write(old)
open(path, 'w').write(new)
print('written')
PY
}

# ── 启动前置：把当前会话的真实参数算出来 ────────────────────────────────

prepare() {
    local dev=""
    [ -n "$(wl_display)" ] || die "没有 Wayland socket（$XDG_RUNTIME_DIR/wayland-*），会话没起来？"
    SIG="$(hypr_sig)" || die "找不到运行中的 Hyprland"
    WLD="$(wl_display)"
    OUT="$(ensure_output "$SIG")"

    dev="$(dmabuf_device || true)"
    [ -n "$dev" ] || dev="$(log_device || true)"
    if [ -n "$dev" ]; then
        DRIVER="$(card_driver "$dev")"
    else
        # 最后兜底：还在用旧的状态文件
        DRIVER="$([ "$(cat "$HOME/.config/hypr/display.state" 2>/dev/null || echo intel)" = nvidia ] && echo nvidia || echo i915)"
        warn "无法从合成器读到抓帧设备，退回 display.state（driver=$DRIVER）"
    fi
    ENC="${SUN_ENCODER:-$(encoder_for_driver "$DRIVER")}"
    CAP="${SUN_CAPTURE:-wlr}"
    LVL="${SUN_LOG_LEVEL:-info}"

    log "会话: $WLD / Hyprland $SIG  输出: $OUT"
    log "抓帧卡: ${dev:-未知}（driver=$DRIVER）→ 编码器 $ENC，抓帧方式 $CAP"

    # 独显 + wlr：Aquamarine 在 NVIDIA 上导入 screencopy dmabuf 必定失败（
    # eglCreateImageKHR EGL_BAD_PARAMETER），客户端只会看到黑屏。直接拒绝，
    # 别让用户对着黑屏猜。kms 需要有真实 CRTC（EDID 诱骗器 / 真显示器）。
    if [ "$DRIVER" != unknown ] && [ "${DRIVER%%[0-9]*}" = nvidia ] && [ "$CAP" = wlr ] \
        && [ "${SUN_FORCE_NVIDIA:-0}" != 1 ]; then
        warn "当前会话在独显（$dev, driver=$DRIVER），Sunshine 的 wlr 抓帧在 NVIDIA 上不可用："
        warn "  Aquamarine 导入 screencopy dmabuf 报 EGL_BAD_PARAMETER（每帧 Frame capture failed）→ Moonlight 黑屏"
        warn "  kms 抓帧也救不了：独显唯一连接器 HDMI-A-1 未接线，HEADLESS 不是真实 CRTC"
        warn "可选：1) scripts/switch-display.sh intel 1920x1080 1   # 已验证可用（1080p60，字小用 font-scale.sh）"
        warn "      2) 给独显 HDMI 口插 EDID 诱骗器，然后 SUN_CAPTURE=kms 启动"
        warn "      3) 明知会黑屏仍要起：SUN_FORCE_NVIDIA=1 $0 start"
        exit 3
    fi
    if [ "$ENC" = nvenc ] && [ "$DRIVER" = unknown ]; then
        warn "抓帧卡未知，用 nvenc 有跨 GPU 风险；如黑屏请跑 $0 doctor"
    fi
    if [ -f "$HOME/.config/hypr/display.state" ]; then
        local st; st="$(cat "$HOME/.config/hypr/display.state" 2>/dev/null || true)"
        [ "$st" = "${DRIVER%%[0-9]*}" ] || [ -z "$st" ] || \
            warn "display.state=$st 与实际 driver=$DRIVER 不一致（已忽略状态文件，按实际会话选 $ENC）"
    fi
}

sunshine_env() {
    printf 'XDG_RUNTIME_DIR=%s WAYLAND_DISPLAY=%s XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=Hyprland' \
        "$XDG_RUNTIME_DIR" "$WLD"
}

distro_unit_active() {
    systemctl --user is-active --quiet "$DISTRO_UNIT" 2>/dev/null \
        || systemctl is-active --quiet "$DISTRO_UNIT" 2>/dev/null
}

check_distro_unit() {
    if distro_unit_active; then
        warn "发行版自带单元 $DISTRO_UNIT 正在运行"
        warn "  它由 user manager 直接拉起，环境是 DISPLAY=:0 / XDG_SESSION_TYPE=x11、"
        warn "  没有 WAYLAND_DISPLAY，Sunshine 抓帧必定失败（Moonlight 全黑）。"
        warn "  先停掉并屏蔽：scripts/install-sunshine-host.sh（或手动 systemctl --user disable --now $DISTRO_UNIT）"
        return 1
    fi
    return 0
}

# ── 子命令 ──────────────────────────────────────────────────────────────

cmd_start() {
    check_distro_unit || die "请先处理上面那个系统单元（或跑 scripts/install-sunshine-host.sh）"
    if systemctl --user is-active --quiet "$SERVICE" 2>/dev/null; then
        die "$SERVICE 已在运行；改用 systemctl --user restart $SERVICE"
    fi
    prepare
    write_conf "$OUT" "$ENC" "$CAP" "$LVL" | sed 's/^/[sun] 配置: /'
    pkill -x sunshine 2>/dev/null || true
    sleep 1
    : > "$SLOG" 2>/dev/null || true
    log "启动 Sunshine（foreground 日志 $OUT_LOG）"
    # setsid：否则启动它的终端/IDE 一退出，Sunshine 会被一起收走
    # env -u DISPLAY：有 DISPLAY 时 Sunshine 会走 X11 分支
    setsid env -u DISPLAY -u XDG_SESSION_TYPE $(sunshine_env) \
        sunshine >>"$OUT_LOG" 2>&1 </dev/null &
    wait_ready || exit 1
}

cmd_run() {
    # systemd 单元入口：等会话 → 写配置 → 前台 exec（进程由 systemd 托管）
    # 开机时本服务和 ww-session 同时被拉起来，等合成器就绪再往下（最多 120s；
    # 超时后 prepare 会报错退出 1，systemd 5s 后重试，不会卡死）
    local i
    for i in $(seq 1 120); do
        if hypr_sig >/dev/null 2>&1 && [ -n "$(wl_display)" ]; then break; fi
        if [ "$i" = 10 ]; then log "等待 Hyprland 会话就绪…（开机时会并行启动，属正常）"; fi
        sleep 1
    done
    prepare
    write_conf "$OUT" "$ENC" "$CAP" "$LVL" | sed 's/^/[sun] 配置: /'
    log "exec sunshine（$ENC / $CAP / $OUT）"
    # 已在 input 组就直接起；否则借组（usermod 后不重新登录也生效）
    if id -nG | tr ' ' '\n' | grep -qx input; then
        exec env -u DISPLAY -u XDG_SESSION_TYPE $(sunshine_env) sunshine
    else
        exec sg input -c "env -u DISPLAY -u XDG_SESSION_TYPE $(sunshine_env) sunshine"
    fi
}

wait_ready() {
    local i pid=""
    for i in $(seq 1 40); do
        if ss -ltn 2>/dev/null | grep -q ':47984'; then break; fi
        sleep 0.5
    done
    pid="$(pgrep -x sunshine | head -1 || true)"
    if ss -ltn 2>/dev/null | grep -q ':47984'; then
        log "就绪：pid=${pid:-?}  encoder=$ENC  Web UI https://127.0.0.1:47990  串流端口 47984-48010"
    else
        warn "47984 未监听，启动可能失败：tail -20 $OUT_LOG"
        tail -20 "$OUT_LOG" 2>/dev/null | cut -c1-200 || true
        return 1
    fi
    if grep -qa 'WAYLAND_DISPLAY has not been defined' "$SLOG" 2>/dev/null; then
        warn "日志显示 Sunshine 没拿到 WAYLAND_DISPLAY（抓帧必失败）"
    fi
    if grep -qa 'Frame capture failed' "$SLOG" 2>/dev/null; then
        warn "日志出现 Frame capture failed（抓帧失败 → 客户端黑屏），跑 $0 doctor"
    fi
    log "抓帧只在 Moonlight 连上后初始化；日志出现 'Screencasting with Wayland' 才算真正打通"
}

# 换分辨率/缩放：虚拟输出的尺寸只能在创建时定，所以「新建一个目标尺寸的输出 →
# 删掉旧的 → 重启 Sunshine」。不能直接删旧输出（最后一个输出被删会让 Hyprland 退出）。
cmd_mode() {
    local res="${1:-}" scale="${2:-1}" sig out next i w h s
    [ -n "$res" ] || die "用法: $0 mode <WxH> [scale]"
    case "$res" in *x*@*) ;; *x*) res="${res%@*}";; *) die "分辨率格式应是 1920x1080" ;; esac
    sig="$(hypr_sig)" || die "找不到运行中的 Hyprland"
    out="$(find_output "$sig")"
    for i in $(seq 1 16); do
        hyprctl_live "$sig" keyword monitor "HEADLESS-$i,$res@60,0x0,$scale" >/dev/null 2>&1 || true
    done
    hyprctl_live "$sig" output create headless >/dev/null 2>&1 || true
    sleep 2
    next="$(list_headless "$sig" | grep -v "^${out}$" | head -1 || true)"
    [ -n "$next" ] || die "新虚拟输出没创建出来（$res@60 scale=$scale）"
    read -r w h s <<< "$(hyprctl_live "$sig" -j monitors 2>/dev/null | python3 -c "
import json,sys
m=[x for x in json.load(sys.stdin) if x['name']=='$next'][0]
print(m['width'], m['height'], m['scale'])")"
    if [ "$w"x"$h" != "$res" ]; then
        warn "新输出实际是 ${w}x${h}（目标 $res）；会话重启后会按 monitors.conf 重新创建"
    fi
    [ -n "$out" ] && hyprctl_live "$sig" output remove "$out" >/dev/null 2>&1 || true
    monitor_conf_write "$next" "$res" "0x0" "$scale"
    log "已切到输出 $next：$res@60 scale=$scale（旧输出 $out 已删除）"
    if systemctl --user is-active --quiet "$SERVICE" 2>/dev/null; then
        systemctl --user restart "$SERVICE"
        log "已重启 $SERVICE 让编码器跟上新尺寸"
    fi
}

cmd_stop() {
    if systemctl --user is-active --quiet "$SERVICE" 2>/dev/null; then
        systemctl --user stop "$SERVICE"
        log "已停止 $SERVICE"
    elif pkill -x sunshine 2>/dev/null; then
        log "Sunshine 已停止"
    else
        log "Sunshine 未在运行"
    fi
}

cmd_restart() {
    if systemctl --user is-active --quiet "$SERVICE" 2>/dev/null \
        || systemctl --user is-enabled --quiet "$SERVICE" 2>/dev/null; then
        systemctl --user restart "$SERVICE"
        sleep 2
        systemctl --user --no-pager status "$SERVICE" | sed -n '1,8p' || true
    else
        cmd_stop; cmd_start
    fi
}

log_stat() { # $1=file $2=pattern
    [ -f "$1" ] || { echo 0; return; }
    grep -ac "$2" "$1" 2>/dev/null || true
}

cmd_status() {
    local pid env
    pid="$(pgrep -x sunshine | head -1 || true)"
    if [ -n "$pid" ]; then
        log "Sunshine 运行中（pid $pid）"
        env="$(tr '\0' '\n' < "/proc/$pid/environ" 2>/dev/null | grep -E '^(WAYLAND_DISPLAY|DISPLAY|XDG_SESSION_TYPE)=' | tr '\n' ' ' || true)"
        echo "  环境: ${env:-（读不到）}"
    else
        log "Sunshine 未运行"
    fi
    echo "  服务: $SERVICE=$(systemctl --user is-active "$SERVICE" 2>/dev/null || true)" \
         " 发行版单元=$(distro_unit_active && echo running || echo off)"
    echo "  配置: $(grep -aE '^(capture|encoder|output_name)\s*=' "$CONF" 2>/dev/null | tr '\n' ' ')"
    echo "  抓帧失败帧数: $(log_stat "$SLOG" 'Frame capture failed')" \
         " Hyprland EGL 错误: $(hypr_egl_errors)"
}

hypr_egl_errors() {
    local n=0 c l
    for l in "$XDG_RUNTIME_DIR"/hypr/*/hyprland.log; do
        [ -f "$l" ] || continue
        c="$(grep -aci 'EGL_BAD_PARAMETER' "$l" 2>/dev/null || true)"
        n=$((n + ${c:-0}))
    done
    echo "$n"
}

cmd_doctor() {
    local sig out dev drv wld enc cap pid
    echo "================ Sunshine / Moonlight 自检 ================"
    wld="$(wl_display || true)"
    echo "1) Wayland socket        : ${wld:-无！会话没起来}"
    if sig="$(hypr_sig)"; then
        echo "2) Hyprland 实例         : $sig"
        if pgrep -af 'Hyprland.*--safe-mode' >/dev/null 2>&1; then
            warn "   注意：当前是 **safe-mode** 会话（崩溃后 watchdog 用 recoverycfg 拉起：无 output 规则、"
            warn "   无 waybar/壁纸/键位）。执行 systemctl --user restart ww-session 恢复正式配置。"
        fi
        hyprctl_live "$sig" -j monitors 2>/dev/null | python3 -c '
import json,sys
for m in json.load(sys.stdin):
    print("   输出 %-12s %sx%s@%.0f scale=%s %s" % (m["name"], m["width"], m["height"],
          m["refreshRate"], m["scale"], "disabled" if m.get("disabled") else "enabled"))' 2>/dev/null || true
    out="$(find_output "$sig" || true)"
    echo "   抓帧输出: ${out:-无}"
    if [ -n "$out" ]; then
        hyprctl_live "$sig" -j monitors 2>/dev/null | python3 -c "
import json,sys
m=[x for x in json.load(sys.stdin) if x['name']=='$out'][0]
print('   dpms=%s  currentFormat=%s' % (m.get('dpmsStatus'), m.get('currentFormat')))" 2>/dev/null || true
    fi
    # 全黑截图的 PNG 会小到几 KB，正常桌面（壁纸+waybar）通常上百 KB
    if WAYLAND_DISPLAY="${wld:-wayland-1}" grim -o "${out:-HEADLESS-1}" /tmp/ww-doctor.png 2>/dev/null; then
        echo "3) 桌面是否真的在画      : grim 成功，PNG $(stat -c %s /tmp/ww-doctor.png) 字节" \
             "（<20KB 基本可判定为全黑）"
    else
        echo "3) 桌面是否真的在画      : grim 失败（没有 WAYLAND_DISPLAY / 合成器异常）"
    fi
    else
        echo "2) Hyprland 实例         : 找不到运行中的 Hyprland"
    fi

    dev="$(dmabuf_device || true)"; [ -n "$dev" ] || dev="$(log_device || true)"
    drv="$(card_driver "${dev:-/dev/dri/card0}")"
    echo "4) 抓帧卡 (dmabuf main)  : ${dev:-未知} driver=${drv}"
    echo "   → 期望编码器          : $(encoder_for_driver "$drv")"
    echo "5) sunshine.conf         : $(grep -aE '^(capture|encoder|output_name|min_log_level)\s*=' "$CONF" 2>/dev/null | tr '\n' ' ')"

    pid="$(pgrep -x sunshine | head -1 || true)"
    if [ -n "$pid" ]; then
        echo "6) Sunshine 进程         : pid=$pid"
        echo "   环境                 : $(tr '\0' '\n' < "/proc/$pid/environ" 2>/dev/null \
            | grep -E '^(WAYLAND_DISPLAY|DISPLAY|XDG_SESSION_TYPE)=' | tr '\n' ' ' || true)"
        echo "   /dev/dri fd 数       : $(for f in /proc/$pid/fd/*; do readlink "$f" 2>/dev/null; done | grep -c dri || true)" \
             "（抓帧走 Wayland EGL 不需要；vaapi/nvenc 编码器打开 renderD* 属正常）"
    else
        echo "6) Sunshine 进程         : 未运行"
    fi
    echo "7) wlr 抓帧日志          : Screencasting=$(log_stat "$SLOG" 'Screencasting with Wayland')" \
         " Monitor=$(log_stat "$SLOG" '] Monitor [0-9]* is')" \
         " 失败帧=$(log_stat "$SLOG" 'Frame capture failed')"
    echo "8) Hyprland EGL 导入错误 : $(hypr_egl_errors) 条 EGL_BAD_PARAMETER"
    echo "9) 服务状态              : $SERVICE=$(systemctl --user is-active "$SERVICE" 2>/dev/null || true)" \
         " 发行版单元=$(distro_unit_active && echo active || echo off)"
    local kms
    kms="$(/usr/sbin/getcap /usr/bin/sunshine 2>/dev/null | sed 's/^.*= //' || true)"
    echo "10) KMS 能力             : ${kms:-未授予（仅 capture=kms 需要：sudo setcap cap_sys_admin+p /usr/bin/sunshine）}"

    echo "----------------------------------------------------------"
    local want ffail conf_enc
    want="$(encoder_for_driver "$drv")"
    ffail="$(log_stat "$SLOG" 'Frame capture failed')"
    conf_enc="$(sed -n 's/^encoder\s*=\s*//p' "$CONF" 2>/dev/null | tail -1)"
    if [ "${drv%%[0-9]*}" = nvidia ]; then
        echo "结论：会话在独显。wlr 抓帧在 NVIDIA + Aquamarine 上不可用（dmabuf 导入 EGL_BAD_PARAMETER）。"
        echo "      要出图：切集显（scripts/switch-display.sh intel 1920x1080 1），或插 EDID 诱骗器后用 kms。"
    elif distro_unit_active; then
        echo "结论：发行版自带单元正在跑（没有 WAYLAND_DISPLAY），它会占住端口并让 Moonlight 黑屏。"
        echo "      执行 scripts/install-sunshine-host.sh 停用并 mask 它。"
    elif [ -n "$conf_enc" ] && [ "$conf_enc" != "$want" ]; then
        echo "结论：配置里的 encoder=$conf_enc 与抓帧卡（driver=$drv，应为 $want）不一致 → 跨 GPU 抓帧会失败。"
        echo "      重启服务让脚本按实际会话重写配置：systemctl --user restart $SERVICE"
    elif [ "$ffail" -gt 0 ] && [ "$(hypr_egl_errors)" -gt 0 ]; then
        echo "结论：抓帧失败且合成器侧有 EGL 导入错误 —— 属于 dmabuf 导入缺陷，不是配置问题。"
    elif [ "$ffail" -gt 0 ]; then
        echo "结论：出现过抓帧失败。先确认 encoder 与抓帧卡同卡（见第 4、5 行），再重启服务。"
    elif [ -z "$pid" ]; then
        echo "结论：Sunshine 没在跑。systemctl --user start $SERVICE"
    else
        echo "结论：环境正常。若客户端仍黑屏，连上后看第 7 行是否出现 Screencasting 且失败帧为 0。"
    fi
}

case "${1:-start}" in
    start)   cmd_start ;;
    run)     cmd_run ;;
    stop)    cmd_stop ;;
    restart) cmd_restart ;;
    mode)    shift; cmd_mode "$@" ;;
    status)  cmd_status ;;
    doctor)  cmd_doctor ;;
    *)
        echo "用法: $0 {start|run|stop|restart|mode <WxH> [scale]|status|doctor}" >&2; exit 1 ;;
esac
