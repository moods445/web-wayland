#!/usr/bin/env bash
# Sunshine / Moonlight 串流全量体检：检测所有已知问题，逐条说明「原因 + 解决步骤」，
# 可选 --fix 自动执行对症修复。现有 scripts/sunshine-wayland.sh doctor 保留不变。
#
# 用法：
#   scripts/doctor.sh           # 只读体检 + 打印所有问题与解决步骤（不改系统）
#   scripts/doctor.sh --fix     # 体检后按优先级自动执行修复（重启会话/服务、切 GPU 等有副作用的动作会标注）
#   scripts/doctor.sh --help
#
# 覆盖 docs/sunshine-moonlight.md §8 故障表 + §11 已修复坑 + docs/bugs/sunshine-nvidia-black-screen.md。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

# IDE 终端注入的私有 libstdc++ 会让 hyprctl / grim / sunshine 等系统二进制起不来
for _p in ${LD_LIBRARY_PATH-}; do
    case "$_p" in
        */.trae-cn-server/*|*/.trae-cn/*) unset LD_LIBRARY_PATH; break ;;
    esac
done

CONF="$HOME/.config/sunshine/sunshine.conf"
SLOG="$HOME/.config/sunshine/sunshine.log"
STATE="$HOME/.config/sunshine/sunshine_state.json"
MONCONF="$HOME/.config/hypr/monitors.conf"
SHOT="/tmp/ww-doctor.png"
DISTRO_UNIT=app-dev.lizardbyte.app.Sunshine.service
SUN=sunshine-wayland.service
SESSION=ww-session.service

FIX=0
case "${1:-}" in
    --fix) FIX=1 ;;
    -h|--help) sed -n '2,11p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    "") ;;
    *) echo "用法: $0 [--fix|--help]" >&2; exit 1 ;;
esac

C_OK=$'\033[32m'; C_WARN=$'\033[33m'; C_FAIL=$'\033[31m'
C_DIM=$'\033[2m'; C_B=$'\033[1m'; C_OFF=$'\033[0m'

N_FAIL=0; N_WARN=0
declare -a FIX_DESC FIX_CMD FIX_HEAVY

ok()      { printf '  %s[ OK ]%s %s\n' "$C_OK" "$C_OFF" "$1"; }
warn()    { printf '  %s[WARN ]%s %s\n' "$C_WARN" "$C_OFF" "$1"; N_WARN=$((N_WARN+1)); }
fail()    { printf '  %s[FAIL ]%s %s\n' "$C_FAIL" "$C_OFF" "$1"; N_FAIL=$((N_FAIL+1)); }
why()     { printf '         %s原因：%s%s\n' "$C_DIM" "$C_OFF" "$1"; }
how()     { printf '         %s解决：%s%s\n' "$C_OK" "$C_OFF" "$1"; }
section() { printf '\n%s== %s ==%s\n' "$C_B" "$1" "$C_OFF"; }
addfix()  { FIX_DESC+=("$1"); FIX_CMD+=("$2"); FIX_HEAVY+=("${3:-0}"); } # desc|cmd|heavy(1=关窗/重建会话)

# ── 探测原语（与 sunshine-wayland.sh 一致）─────────────────────────────────
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

hc() { HYPRLAND_INSTANCE_SIGNATURE="$SIG" hyprctl "$@" 2>/dev/null; }

dmabuf_device() {
    command -v wayland-info >/dev/null || return 1
    wayland-info 2>/dev/null \
        | sed -n '/interface: .zwp_linux_dmabuf_v1/,/^interface:/p' \
        | sed -n 's/.*main device:.*\(\/dev\/dri\/[a-z]*[0-9]*\).*/\1/p' | head -1
}
log_device() {
    local l; l="$(ls -t "$XDG_RUNTIME_DIR"/hypr/*/hyprland.log 2>/dev/null | head -1)"
    [ -n "$l" ] || return 1
    sed -n 's/.*Starting backend for \(\/dev\/dri\/card[0-9]*\).*/\1/p' "$l" | tail -1
}
card_driver() {
    local node="/sys/class/drm/$(basename "${1:-}")/device/driver"
    [ -e "$node" ] && basename "$(readlink -f "$node")" || echo unknown
}
encoder_for_driver() {
    case "$1" in
        nvidia*)                echo nvenc ;;
        i915|xe|amdgpu|radeon*) echo vaapi ;;
        *)                      echo vaapi ;;
    esac
}
conf_val() { sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*//p" "$CONF" 2>/dev/null | tail -1; }
# grep -c 无匹配时退出码非 0，会连带 || echo 0 再输出一次 0 → 得到 "0\n0"；用 if 显式处理
count()    { if [ -f "$1" ]; then local n; n="$(grep -ac "$2" "$1" 2>/dev/null)"; echo "${n:-0}"; else echo 0; fi; }
en_weight() { local w="$1"; case "$w" in *nvidia*) echo nvidia;; *) echo "$w";; esac; }

SIG="$(hypr_sig || true)"
WLD="$(wl_display || true)"

printf '%s================ Sunshine / Moonlight 全量体检 ================%s\n' "$C_B" "$C_OFF"
echo "时间: $(date '+%F %T')   用户: $(id -un)   XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR"

# ── 1. 会话 ────────────────────────────────────────────────────────────────
section "1. 桌面会话"
if [ -z "$WLD" ]; then
    fail "没有 Wayland socket（$XDG_RUNTIME_DIR/wayland-*）"
    why "Hyprland 会话没起来，Sunshine 的 wlr 抓帧拿不到合成器 → 全黑"
    how "systemctl --user restart $SESSION"
    addfix "重启桌面会话（恢复 Wayland socket）" "systemctl --user restart $SESSION" 1
else
    ok "Wayland socket: $WLD"
fi

if [ -z "$SIG" ]; then
    fail "找不到运行中的 Hyprland 实例"
    why "没有合成器就没有 screencopy，Sunshine 必然全黑"
    how "systemctl --user restart $SESSION; 若反复崩溃看 journalctl --user -u $SESSION -n 50"
    addfix "重启桌面会话" "systemctl --user restart $SESSION" 1
else
    ok "Hyprland 实例: $SIG"
fi

SAFEMODE=0
if pgrep -af 'Hyprland.*--safe-mode' >/dev/null 2>&1; then SAFEMODE=1; fi
if [ -n "$SIG" ] && hc -j clients 2>/dev/null | grep -q '"class":"hyprland-dialog"' 2>/dev/null; then
    # 安全模式对话框也作为辅证（有则更确定）
    SAFEMODE=1
fi
if [ "$SAFEMODE" = 1 ]; then
    fail "当前是 safe-mode 会话（崩溃后 watchdog 用 recoverycfg.conf 拉起）"
    why "安全模式不加载正式配置：物理屏 eDP-1 被点亮、无壁纸/waybar/键位，输出布局错乱；多个输出时 screencopy 会抓到错层 → Moonlight 黑屏"
    how "systemctl --user restart $SESSION 退出安全模式，再 systemctl --user restart $SUN 让抓帧重新初始化"
    addfix "重启会话退出 safe-mode（桌面窗口会关闭）" "systemctl --user restart $SESSION; sleep 3; systemctl --user restart $SUN" 1
else
    ok "非 safe-mode 会话"
fi

HYP_PIDS="$(pgrep -x Hyprland 2>/dev/null | wc -l)"
if [ "$HYP_PIDS" -gt 1 ]; then
    fail "检测到 $HYP_PIDS 个 Hyprland 实例（嵌套会话）"
    why "手工起的 Hyprland 占着 DRM master，新实例退化成嵌套：多一个 WAYLAND-N 伪输出，画面合成两遍、鼠标位移减半"
    how "不要手工起 Hyprland；systemctl --user restart $SESSION（ExecStartPre 会清残留实例）"
    addfix "重启会话清理嵌套实例" "systemctl --user restart $SESSION; sleep 3; systemctl --user restart $SUN" 1
else
    ok "Hyprland 实例数: 1"
fi

# ── 2. 输出布局 ────────────────────────────────────────────────────────────
section "2. 虚拟输出与布局"
MON_TSV=""
if [ -n "$SIG" ]; then
    MON_TSV="$(hc -j monitors 2>/dev/null | python3 -c '
import json,sys
for m in json.load(sys.stdin):
    print("\t".join([m["name"], str(m["width"]), str(m["height"]), str(m["scale"]),
                     str(m["x"]), str(m["y"]), "1" if m.get("disabled") else "0",
                     str(m.get("dpmsStatus"))]))' 2>/dev/null || true)"
fi

HEADLESS_COUNT=0; OUT=""; EDP_ENABLED=0
if [ -n "$MON_TSV" ]; then
    while IFS=$'\t' read -r n w h sc x y dis dpms; do
        [ -z "$n" ] && continue
        case "$n" in HEADLESS*) [ "$dis" = 0 ] && HEADLESS_COUNT=$((HEADLESS_COUNT+1)) && [ -z "$OUT" ] && OUT="$n";; esac
        case "$n" in eDP-1|HDMI-A-1) [ "$dis" = 0 ] && EDP_ENABLED=1;; esac
        printf '  %-12s %sx%s  scale=%s  at %s,%s  %s  dpms=%s\n' "$n" "$w" "$h" "$sc" "$x" "$y" \
            "$([ "$dis" = 1 ] && echo disabled || echo enabled)" "$dpms"
    done <<< "$MON_TSV"
else
    fail "读不到显示器列表（hyprctl -j monitors）"
    why "无法确认抓帧输出，通常是会话异常"
    how "systemctl --user restart $SESSION"
fi

if [ "$HEADLESS_COUNT" -eq 0 ]; then
    fail "没有可用的 HEADLESS 虚拟输出"
    why "本机桌面只存在于 headless 输出（eDP-1 被 disable）。没有它 Sunshine 无处可抓 → 全黑"
    how "systemctl --user restart $SUN（脚本启动时会自动创建 headless 输出）；仍不行则 systemctl --user restart $SESSION"
    addfix "重启 Sunshine 让其重建 headless 输出" "systemctl --user restart $SUN" 0
elif [ "$HEADLESS_COUNT" -gt 1 ]; then
    fail "检测到 $HEADLESS_COUNT 个 HEADLESS 输出（同坐标重叠会让 screencopy 抓错层 → 黑屏）"
    how "systemctl --user restart $SUN（启动脚本会删掉多余的虚拟输出，只保留一个）"
    addfix "重启 Sunshine 清理多余虚拟输出" "systemctl --user restart $SUN" 0
else
    ok "虚拟输出: $OUT（唯一）"
fi

if [ "$EDP_ENABLED" = 1 ]; then
    fail "物理屏 eDP-1 处于 enabled（正式配置应为 monitor = eDP-1, disable）"
    why "物理屏参与布局会与 headless 争抢 screencopy；safe-mode 或手工改 monitor 时会出现，表现为 Moonlight 黑屏/抓错层"
    how "退出 safe-mode：systemctl --user restart $SESSION（正式配置会 disable eDP-1）"
    addfix "重启会话恢复正式显示器配置（eDP-1 disable）" "systemctl --user restart $SESSION; sleep 3; systemctl --user restart $SUN" 1
else
    ok "物理屏 eDP-1 已禁用（设计选择）"
fi

# 同坐标重叠检测（任意两个 enabled 输出坐标相同）
if [ -n "$MON_TSV" ]; then
    OVERLAP="$(printf '%s\n' "$MON_TSV" | awk -F'\t' '$7==0{c[$5","$6]++; if(c[$5","$6]>1) print $5","$6}' | head -1)"
    if [ -n "$OVERLAP" ]; then
        fail "多个启用输出同坐标 ($OVERLAP)：screencopy 会抓到错层 → 黑屏"
        how "重启会话让 headless 与物理屏分开（正式配置只留 headless）；或 hyprctl -j monitors 查看并重排"
        addfix "重启会话消除输出重叠" "systemctl --user restart $SESSION; sleep 3; systemctl --user restart $SUN" 1
    else
        ok "输出坐标无重叠"
    fi
fi

# ── 3. 桌面是否真的在画 ────────────────────────────────────────────────────
section "3. 桌面渲染"
if [ -n "$WLD" ] && [ -n "$OUT" ]; then
    if WAYLAND_DISPLAY="$WLD" HYPRLAND_INSTANCE_SIGNATURE="$SIG" grim -o "$OUT" "$SHOT" >/dev/null 2>&1; then
        BYTES="$(stat -c %s "$SHOT" 2>/dev/null || echo 0)"
        if [ "$BYTES" -lt 20000 ]; then
            fail "grim 截图仅 $BYTES 字节，抓帧输出 $OUT 几乎全黑（壁纸/waybar 没画出来）"
            why "合成器没有真正渲染桌面：safe-mode 无壁纸、swaybg/hyprpaper 挂了，或输出 dpms 关闭"
            how "systemctl --user restart $SESSION 恢复正式配置（含 swaybg 壁纸 + waybar）"
            addfix "重启会话恢复桌面渲染" "systemctl --user restart $SESSION; sleep 3; systemctl --user restart $SUN" 1
        else
            ok "抓帧输出 $OUT 渲染正常（grim 截图 $BYTES 字节）"
        fi
    else
        warn "grim 截图失败（无 WAYLAND_DISPLAY / 合成器异常），无法判断桌面是否在画"
    fi
fi

# ── 4. 抓帧卡 / 编码器 ─────────────────────────────────────────────────────
section "4. 抓帧卡与编码器"
DEV="$(dmabuf_device || true)"; [ -n "$DEV" ] || DEV="$(log_device || true)"
DRV="$(card_driver "${DEV:-/dev/dri/card0}")"
WANT="$(encoder_for_driver "$DRV")"
echo "  抓帧卡(dmabuf main): ${DEV:-未知}   driver=$DRV   期望编码器=$WANT"

CONF_CAP="$(conf_val capture)"; CONF_ENC="$(conf_val encoder)"; CONF_OUT="$(conf_val output_name)"
echo "  sunshine.conf: capture=$CONF_CAP encoder=$CONF_ENC output_name=$CONF_OUT"

if [ "$CONF_CAP" != wlr ]; then
    case "$CONF_CAP" in
        wlgrab) why_="该构建不认识 wlgrab，合法值是 wlr（写 wlgrab 会 Unable to initialize capture method）" ;;
        kms)    why_="capture=kms 只认真实 CRTC；本机独显 HDMI-A-1 未接线、headless 不是真实连接器，KMS 列表为空" ;;
        *)      why_="capture 应为 wlr（Wayland screencopy）；当前值 $CONF_CAP 不受支持" ;;
    esac
    fail "capture=$CONF_CAP 配置有误"
    why "$why_"
    how "systemctl --user restart $SUN（脚本会重写为 capture=wlr）"
    addfix "重启 Sunshine 重写 capture=wlr" "systemctl --user restart $SUN" 0
fi

if [ -n "$CONF_ENC" ] && [ "$CONF_ENC" != "$WANT" ]; then
    fail "编码器 $CONF_ENC 与抓帧卡（$DRV，应为 $WANT）不同卡 → 跨 GPU 导入 dmabuf 失败 → 每帧 Frame capture failed、全黑"
    how "systemctl --user restart $SUN（脚本按 dmabuf 主设备的驱动重写 encoder）"
    addfix "重启 Sunshine 纠正编码器为 $WANT" "systemctl --user restart $SUN" 0
else
    ok "编码器与抓帧卡同卡（$CONF_ENC）"
fi

if [ -n "$OUT" ] && [ -n "$CONF_OUT" ] && [ "$CONF_OUT" != "$OUT" ]; then
    fail "sunshine.conf 的 output_name=$CONF_OUT 与实际 headless 输出 $OUT 不一致 → 抓到不存在的层 → 黑屏"
    how "systemctl --user restart $SUN（脚本按实际输出名重写配置）"
    addfix "重启 Sunshine 重写 output_name=$OUT" "systemctl --user restart $SUN" 0
else
    ok "output_name 与实际输出一致（${CONF_OUT:-未设置}）"
fi

if [ "$(en_weight "$DRV")" = nvidia ]; then
    fail "会话跑在独显（nvidia），Sunshine 的 wlr 抓帧在本机虚拟输出上不可用（未修复缺陷）"
    why "Aquamarine 导入 screencopy dmabuf 报 EGL_BAD_PARAMETER，每帧 Frame capture failed → 黑屏；换 nvenc 或 kms 都救不了"
    how "回集显：scripts/switch-display.sh intel 1920x1080 1（已验证 1080p60）；要 4K60 需给独显 HDMI 插 EDID 诱骗器后改 capture=kms"
    addfix "切回集显 1080p（会重建会话、关闭桌面窗口）" "$ROOT/scripts/switch-display.sh intel 1920x1080 1" 1
fi

# ── 5. 进程与环境 ──────────────────────────────────────────────────────────
section "5. Sunshine 进程环境"
PID="$(pgrep -x sunshine | head -1 || true)"
if [ -z "$PID" ]; then
    fail "Sunshine 未运行"
    how "systemctl --user start $SUN"
    addfix "启动 Sunshine 服务" "systemctl --user start $SUN" 0
else
    ok "Sunshine 运行中（pid=$PID）"
    ENV="$(tr '\0' '\n' < "/proc/$PID/environ" 2>/dev/null || true)"
    HAS_WLD="$(printf '%s\n' "$ENV" | grep -c '^WAYLAND_DISPLAY=' || true)"
    HAS_DISP="$(printf '%s\n' "$ENV" | grep -c '^DISPLAY=' || true)"
    if [ "$HAS_WLD" -eq 0 ]; then
        fail "Sunshine 进程没有 WAYLAND_DISPLAY（抓帧必失败 → 全黑）"
        why "发行版单元 app-dev...Sunshine 由 user manager 以 DISPLAY=:0/x11 环境拉起，拿不到 Wayland socket"
        how "scripts/install-sunshine-host.sh（停用并 mask 发行版单元，改由 $SUN 带正确环境启动）"
        addfix "停用发行版单元并重装 host 部署" "$ROOT/scripts/install-sunshine-host.sh --restart-session" 1
    else
        ok "进程带 WAYLAND_DISPLAY（$(printf '%s\n' "$ENV" | grep '^WAYLAND_DISPLAY=' | head -1)）"
    fi
    if [ "$HAS_DISP" -gt 0 ]; then
        fail "Sunshine 环境残留 DISPLAY=$(printf '%s\n' "$ENV" | grep '^DISPLAY=' | head -1 | cut -d= -f2)（会走 X11 分支 → 抓帧失败）"
        how "scripts/install-sunshine-host.sh --restart-session（脚本内部 env -u DISPLAY，并清理 user manager 的 DISPLAY）"
        addfix "清理 X11 残留环境并重启" "$ROOT/scripts/install-sunshine-host.sh --restart-session" 1
    fi
fi

if systemctl --user is-active --quiet "$DISTRO_UNIT" 2>/dev/null || systemctl is-active --quiet "$DISTRO_UNIT" 2>/dev/null; then
    fail "发行版单元 $DISTRO_UNIT 正在运行（没有 WAYLAND_DISPLAY，会占 47984-47990 端口并让 Moonlight 黑屏）"
    how "scripts/install-sunshine-host.sh（disable + mask 它）"
    addfix "停用并 mask 发行版 Sunshine 单元" "$ROOT/scripts/install-sunshine-host.sh --restart-session" 1
else
    ok "发行版 Sunshine 单元未运行"
fi

# ── 6. 抓帧日志 ────────────────────────────────────────────────────────────
section "6. 抓帧日志与错误计数"
SCREENCAST="$(count "$SLOG" 'Screencasting with Wayland')"
FFAIL="$(count "$SLOG" 'Frame capture failed')"
EGLN=0
for l in "$XDG_RUNTIME_DIR"/hypr/*/hyprland.log; do
    [ -f "$l" ] || continue
    local_n="$(grep -aci 'EGL_BAD_PARAMETER' "$l" 2>/dev/null)"
    EGLN=$((EGLN + ${local_n:-0}))
done
echo "  Screencasting=$SCREENCAST  失败帧=$FFAIL  Hyprland EGL_BAD_PARAMETER=$EGLN"

if grep -qa 'WAYLAND_DISPLAY has not been defined' "$SLOG" 2>/dev/null; then
    fail "日志显示 Sunshine 没拿到 WAYLAND_DISPLAY"
    how "scripts/install-sunshine-host.sh --restart-session"
    addfix "重装 host 部署修正环境" "$ROOT/scripts/install-sunshine-host.sh --restart-session" 1
fi
if [ "$FFAIL" -gt 0 ] && [ "$EGLN" -gt 0 ]; then
    fail "抓帧失败 $FFAIL 帧且合成器侧 EGL 导入错误 $EGLN 条 → dmabuf 导入缺陷（多为独显 + wlr）"
    how "切集显 scripts/switch-display.sh intel 1920x1080 1；或确认抓帧卡与编码器同卡后 systemctl --user restart $SUN"
    addfix "切回集显 1080p（会重建会话）" "$ROOT/scripts/switch-display.sh intel 1920x1080 1" 1
elif [ "$FFAIL" -gt 0 ]; then
    fail "日志出现 $FFAIL 次 Frame capture failed（抓帧失败 → 客户端黑屏）"
    why "常见原因：编码器与抓帧卡不同卡、output_name 失配、多个输出重叠"
    how "先确认上面第 4/5 节都 OK，再 systemctl --user restart $SUN"
    addfix "重启 Sunshine 重新初始化抓帧" "systemctl --user restart $SUN" 0
else
    ok "无 Frame capture failed 记录"
fi

if [ "$SCREENCAST" -eq 0 ]; then
    warn "日志还没有 Screencasting with Wayland（抓帧只在 Moonlight 连上后初始化，属正常；若连接后仍为 0 则是没真正连上）"
fi

# ── 7. 互斥服务 / 输入 / 配对 ──────────────────────────────────────────────
section "7. 互斥服务、输入与配对"
if systemctl --user is-active --quiet ww-cap 2>/dev/null || pgrep -x wwcap >/dev/null 2>&1; then
    fail "wwcap 正在运行：与 Sunshine 双抓帧，帧率腰斩且互相干扰"
    how "systemctl --user stop ww-cap; pkill -x wwcap"
    addfix "停止 wwcap 避免双抓帧" "systemctl --user stop ww-cap 2>/dev/null; pkill -x wwcap 2>/dev/null || true" 0
else
    ok "wwcap 未运行"
fi

if ! id -nG 2>/dev/null | tr ' ' '\n' | grep -qx input; then
    fail "用户 $(id -un) 不在 input 组：键鼠注入（/dev/uinput root:input 0660）不生效"
    how "sudo usermod -aG input $(id -un)，然后重新登录（或本次会话用 sg input 启动 Sunshine）"
    warn "此问题无法由 --fix 自动完成（需 sudo + 重新登录），请手工执行上面命令"
else
    ok "用户在 input 组（键鼠注入可用）"
fi

NC=0
if [ -f "$STATE" ]; then
    NC="$(python3 -c 'import json;print(len(json.load(open("'"$STATE"'")).get("named_devices",[])))' 2>/dev/null || echo 0)"
fi
if [ "$NC" -eq 0 ]; then
    warn "Sunshine 尚无已配对设备（named_devices=0）：客户端需在 https://<主机IP>:47990 用 Moonlight 显示的 PIN 配对一次才能串流"
    how "Moonlight 添加主机 → 连一下拿到 PIN → 浏览器打开 https://<IP>:47990 提交 PIN 完成配对"
else
    ok "已配对设备数: $NC"
fi

# 透明光标主题残留（Sunshine 下光标应由合成器画进视频帧，不该用浏览器链路的透明主题）
# 只认「生效的 Environment= 行」，别匹配到注释里的「不要设 XCURSOR_THEME=ww-invisible」；
# /proc/<pid>/environ 普通用户读不到别的进程，能读时才查（读不到不算异常）。
CURSOR_BAD=0
UNIT_FILE="$HOME/.config/systemd/user/${SESSION}"
if grep -qE '^[[:space:]]*Environment[[:space:]]*=[[:space:]]*XCURSOR_THEME[[:space:]]*=[[:space:]]*ww-invisible' "$UNIT_FILE" 2>/dev/null; then
    CURSOR_BAD=1
fi
for pid in $(pgrep -x Hyprland 2>/dev/null); do
    if [ -r "/proc/$pid/environ" ]; then
        if tr '\0' '\n' < "/proc/$pid/environ" 2>/dev/null | grep -q '^XCURSOR_THEME=ww-invisible$'; then
            CURSOR_BAD=1
        fi
    fi
done
if [ "$CURSOR_BAD" -eq 1 ]; then
    fail "会话仍带浏览器链路的全透明光标主题 ww-invisible：Sunshine 抓帧看不到鼠标指针"
    how "scripts/install-sunshine-host.sh --restart-session（去掉 XCURSOR_THEME、打开 enable_hyprcursor）"
    addfix "去掉透明光标主题" "$ROOT/scripts/install-sunshine-host.sh --restart-session" 1
else
    ok "无透明光标主题残留"
fi

# ── 8. 端口 ────────────────────────────────────────────────────────────────
section "8. 监听端口"
if ss -ltn 2>/dev/null | grep -q ':47984'; then
    ok "47984 已监听（Web UI 47990 / 控制 47989 / RTSP 48010）"
else
    fail "47984 未监听：Sunshine 没起来或被发行版单元抢占"
    how "systemctl --user restart $SUN；确认发行版单元已 mask"
    addfix "重启 Sunshine 恢复监听" "systemctl --user restart $SUN" 0
fi

# ── 汇总 ───────────────────────────────────────────────────────────────────
printf '\n%s================ 体检结论 ================%s\n' "$C_B" "$C_OFF"
echo "FAIL=$N_FAIL  WARN=$N_WARN"
if [ "$N_FAIL" -eq 0 ] && [ "$N_WARN" -eq 0 ]; then
    echo "未发现问题。若 Moonlight 仍黑屏，连上后确认日志第 7 节 Screencasting 增长、失败帧为 0，并核对客户端是否已完成配对。"
else
    echo "修复动作（按检测顺序，数字越大越靠后执行）："
    if [ "${#FIX_DESC[@]}" -eq 0 ]; then
        echo "  （无自动修复项，请按上面各条「解决」手工处理，如配对 / input 组）"
    else
        # 去重：相同命令只保留一次
        declare -A seen
        for i in "${!FIX_DESC[@]}"; do
            cmd="${FIX_CMD[$i]}"
            [ -n "${seen[$cmd]:-}" ] && continue
            seen[$cmd]=1
            mark=""; [ "${FIX_HEAVY[$i]}" = 1 ] && mark="  ${C_WARN}[会关窗口/重建会话]${C_OFF}"
            printf '  %d. %s%s\n      $ %s\n' "${#seen[@]}" "${FIX_DESC[$i]}" "$mark" "$cmd"
        done
    fi
    if [ "$FIX" -eq 1 ]; then
        printf '\n%s--fix：开始执行修复...%s\n' "$C_B" "$C_OFF"
        declare -A done
        for i in "${!FIX_DESC[@]}"; do
            cmd="${FIX_CMD[$i]}"
            [ -n "${done[$cmd]:-}" ] && continue
            done[$cmd]=1
            printf '%s[FIX]%s %s\n' "$C_OK" "$C_OFF" "${FIX_DESC[$i]}"
            printf '      $ %s\n' "$cmd"
            if bash -c "$cmd"; then
                printf '      %s完成%s\n' "$C_OK" "$C_OFF"
            else
                printf '      %s失败（继续后续修复）%s\n' "$C_FAIL" "$C_OFF"
            fi
        done
        echo "修复已执行完毕。请重新运行 scripts/doctor.sh 复查，并在 Moonlight 里重连确认出图。"
    else
        echo "如需自动修复：scripts/doctor.sh --fix   （标注「会关窗口/重建会话」的动作会关闭当前桌面程序）"
    fi
fi
