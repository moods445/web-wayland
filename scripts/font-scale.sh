#!/usr/bin/env bash
# 1080p@1x 下的「字号放大」方案：只放大字号，不缩放画面。
#
# 为什么不能靠 scale：
#   Hyprland scale=2 / 分数缩放、GDK_SCALE、QT_SCALE_FACTOR、Chrome
#   --force-device-scale-factor≠1 都会让画面先渲染到更大的缓冲区、再由合成器或客户端
#   重采样回 1920×1080 —— 文字边缘变成 2×2 的灰阶块（发虚）。
#   把字号调大则是让文字**按目标字号直接栅格化到原生 1920×1080 像素**上，笔画仍是 1px 级过渡。
#
# 用法：
#   scripts/font-scale.sh                # 应用默认档位 1.3
#   scripts/font-scale.sh 1.0            # 回滚到原始字号
#   scripts/font-scale.sh 1.15|1.3|1.45  # 三档常用值
#   scripts/font-scale.sh status         # 看各程序当前字号 + 有没有"整屏缩放"糊源
#
# 档位 k 的作用范围（全部是字号/尺寸，不含任何画面缩放）：
#   GTK 程序   text-scaling-factor = k            （只放大字号，界面按新字号重绘）
#   foot       pixelsize = 14 * k                 （像素单位，绕开 DPI 推算）
#   fuzzel     size      = 11 * k                 （窗口尺寸随字号自然变大）
#   waybar     font-size = 12 * k px、height 同比（图标是字体字形，一起变大）
#   光标       cursor:size = 24 * k               （否则大字环境下指针显得过小）
#   Chrome     不走缩放：见 docs（1x DSF + 页面缩放 125%）
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOME_CFG="${XDG_CONFIG_HOME:-$HOME/.config}"

HYPR_CONF="$HOME_CFG/hypr/hyprland.conf"
WAYBAR_JSON="$HOME_CFG/waybar/config.jsonc"
WAYBAR_CSS="$HOME_CFG/waybar/style.css"
FUZZEL_INI="$HOME_CFG/fuzzel/fuzzel.ini"
FOOT_INI="$HOME_CFG/foot/foot.ini"

# 基准值：k=1.0 时就是当前桌面（改动前）的观感
BASE_GTK=11        # Cantarell 11（仅用于 status 展示）
BASE_WAYBAR_PX=12
BASE_WAYBAR_H=38
BASE_FUZZEL=11
BASE_FOOT_PX=14
BASE_CURSOR=24
BASE_FZ_HPAD=18
BASE_FZ_VPAD=14
BASE_FZ_INNER=10
BASE_FZ_LINEH=26

log()  { printf '\033[1;34m[font]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[font]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[font]\033[0m %s\n' "$*" >&2; exit 1; }

# 四舍五入的整数乘法：calc <基准> <k>
calc() { awk -v b="$1" -v k="$2" 'BEGIN { printf "%d", int(b * k + 0.5) }'; }

backup() {
    local f="$1"
    [ -f "$f" ] || return 0
    [ -f "$f.bak" ] || cp -f "$f" "$f.bak"
}

have() { command -v "$1" >/dev/null 2>&1; }

# ── 各处写入（幂等）─────────────────────────────────────────────────────

set_gtk() {
    local k="$1"
    # 只放大字号；不要同时改 font-name（会和缩放因子相乘，界面直接错位）
    have gsettings && gsettings set org.gnome.desktop.interface text-scaling-factor "$k" 2>/dev/null || true
    # Wayland 下没有子像素渲染，grayscale 抗锯齿 + full hinting 小字号最清晰
    have gsettings && gsettings set org.gnome.desktop.interface font-antialiasing 'grayscale' 2>/dev/null || true
    have gsettings && gsettings set org.gnome.desktop.interface font-hinting 'full' 2>/dev/null || true

    # 同样的抗锯齿设置写进 gtk-3.0/4.0（GTK 无 dconf 时也生效；只补这几个键，不动别的）
    local v
    for v in 3.0 4.0; do
        local d="$HOME_CFG/gtk-$v" f="$HOME_CFG/gtk-$v/settings.ini"
        mkdir -p "$d"
        python3 - "$f" <<'PY'
import os, sys
p = sys.argv[1]
want = {
    'gtk-xft-antialias': '1',
    'gtk-xft-hinting': '1',
    'gtk-xft-hintstyle': 'hintfull',
    'gtk-xft-rgba': 'none',      # Wayland: 无子像素，等价 grayscale
}
lines = open(p).read().splitlines() if os.path.exists(p) else ['[Settings]']
out, seen, in_settings = [], set(), False
for ln in lines:
    s = ln.strip()
    if s.startswith('['):
        in_settings = (s == '[Settings]')
        out.append(ln); continue
    if in_settings:
        key = s.split('=', 1)[0].strip()
        if key in want:
            out.append(f'{key}={want[key]}'); seen.add(key); continue
    out.append(ln)
missing = [f'{k}={v}' for k, v in want.items() if k not in seen]
if missing:
    if '[Settings]' not in [l.strip() for l in out]:
        out.insert(0, '[Settings]')
    idx = [l.strip() for l in out].index('[Settings]')
    out[idx+1:idx+1] = missing
open(p, 'w').write('\n'.join(out) + '\n')
PY
    done
}

set_foot() {
    local px="$1"
    mkdir -p "$(dirname "$FOOT_INI")"
    backup "$FOOT_INI"
    # dpi-aware=no：字号用像素表达，避免 foot 再按 DPI 换算导致半像素发虚
    cat > "$FOOT_INI" <<EOF
# 由 scripts/font-scale.sh 生成（git 里对应 config/foot/foot.ini）
# 只放大字号：pixelsize 直接落在原生 1920×1080 像素上，配合 dpi-aware=no 保持清晰
[main]
font=monospace:pixelsize=$px
dpi-aware=no
pad=6x6
EOF
}

set_fuzzel() {
    local size="$1" hpad="$2" vpad="$3" inner="$4" lineh="$5"
    [ -f "$FUZZEL_INI" ] || { warn "没有 $FUZZEL_INI，跳过 fuzzel"; return 0; }
    backup "$FUZZEL_INI"
    python3 - "$FUZZEL_INI" "$size" "$hpad" "$vpad" "$inner" "$lineh" <<'PY'
import re, sys
p, size, hpad, vpad, inner, lineh = sys.argv[1:7]
t = open(p).read()
t = re.sub(r'(?m)^font=.*$', f'font=Noto Sans CJK SC:size={size}', t, count=1)
for key, val in (('horizontal-pad', hpad), ('vertical-pad', vpad),
                 ('inner-pad', inner), ('line-height', lineh)):
    if re.search(rf'(?m)^{key}=', t):
        t = re.sub(rf'(?m)^{key}=.*$', f'{key}={val}', t, count=1)
open(p, 'w').write(t)
PY
}

set_waybar() {
    local px="$1" h="$2"
    [ -f "$WAYBAR_CSS" ] || { warn "没有 $WAYBAR_CSS，跳过 waybar"; return 0; }
    backup "$WAYBAR_CSS"; backup "$WAYBAR_JSON"
    python3 - "$WAYBAR_CSS" "$WAYBAR_JSON" "$px" "$h" <<'PY'
import re, sys
css, js, px, h = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
t = open(css).read()
# CSS 的 px 优先级高于 GTK text-scaling-factor，所以 waybar 必须显式改（否则顶栏不变）
t2 = re.sub(r'font-size:\s*[\d.]+px', f'font-size: {px}px', t, count=1)
open(css, 'w').write(t2)
import os
if os.path.exists(js):
    j = open(js).read()
    j2 = re.sub(r'"height":\s*\d+', f'"height": {h}', j, count=1)
    open(js, 'w').write(j2)
PY
}

# 指针尺寸：Hyprland 没有 cursor:size 选项，只能靠启动时的 XCURSOR_SIZE /
# HYPRCURSOR_SIZE 环境变量（ww-session.service），所以这里改单元文件；
# 值变了就重启会话（Sunshine 会随 PartOf 一起重建）。
set_cursor() {
    local size="$1" u old=""
    for u in "$HOME_CFG/systemd/user/ww-session.service" "$ROOT/scripts/ww-session.service"; do
        [ -f "$u" ] || continue
        old="$(grep -oE '^Environment=XCURSOR_SIZE=[0-9]+' "$u" 2>/dev/null | grep -oE '[0-9]+$' || true)"
        # 仓库里的那份由 git 管，不留 .bak；只备份用户目录下的已安装单元
        case "$u" in "$HOME_CFG"/*) backup "$u" ;; esac
        python3 - "$u" "$size" <<'PY'
import re, sys
p, size = sys.argv[1], sys.argv[2]
t = open(p).read()
for key in ('XCURSOR_SIZE', 'HYPRCURSOR_SIZE'):
    pat = rf'(?m)^Environment={key}=.*$'
    if re.search(pat, t):
        t = re.sub(pat, f'Environment={key}={size}', t, count=1)
    else:
        t = re.sub(r'(?m)^\[Service\][ \t]*$',
                   f'[Service]\nEnvironment={key}={size}', t, count=1)
open(p, 'w').write(t)
PY
    done
    if [ -n "$old" ] && [ "$old" != "$size" ]; then
        if systemctl --user is-active --quiet ww-session 2>/dev/null; then
            log "光标 ${old}→${size}px 需要重启会话生效（Sunshine 会跟着重建）"
            systemctl --user restart ww-session 2>/dev/null || true
        fi
    fi
}

reload_desktop() {
    local sig wld
    export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
    wld="$(ls "$XDG_RUNTIME_DIR" 2>/dev/null | grep -E '^wayland-[0-9]+$' | head -1 || true)"
    [ -n "$wld" ] && export WAYLAND_DISPLAY="$wld"
    sig="$(ls -dt "$XDG_RUNTIME_DIR"/hypr/*/ 2>/dev/null | head -1 | xargs -r basename || true)"
    if [ -n "$sig" ] && HYPRLAND_INSTANCE_SIGNATURE="$sig" hyprctl version >/dev/null 2>&1; then
        HYPRLAND_INSTANCE_SIGNATURE="$sig" hyprctl reload >/dev/null 2>&1 || true
        log "已 hyprctl reload（光标尺寸生效）"
    fi
    # waybar 不监听配置变化，必须重启（systemd 单元优先，避免出现两份）
    if systemctl --user is-active --quiet waybar 2>/dev/null; then
        systemctl --user restart waybar 2>/dev/null || true
        log "已重启 waybar（systemd 单元）"
    elif pgrep -x waybar >/dev/null 2>&1; then
        pkill -x waybar 2>/dev/null || true
        sleep 0.3
        setsid waybar >/dev/null 2>&1 </dev/null &
        log "已重启 waybar"
    fi
}

# ── 子命令 ──────────────────────────────────────────────────────────────

apply() {
    local k="$1"
    case "$k" in
        1|1.0|1.00) k=1.0 ;;
    esac
    awk -v k="$k" 'BEGIN { exit !(k >= 1.0 && k <= 2.0) }' || die "档位应在 1.0 ~ 2.0 之间（当前 $k）"

    local wb_px wb_h fz foot_px cursor
    wb_px="$(calc "$BASE_WAYBAR_PX" "$k")"
    wb_h="$((BASE_WAYBAR_H + 2 * (wb_px - BASE_WAYBAR_PX)))"     # 字变大 → 栏高同比涨
    fz="$(calc "$BASE_FUZZEL" "$k")"
    foot_px="$(calc "$BASE_FOOT_PX" "$k")"
    cursor="$(calc "$BASE_CURSOR" "$k")"

    log "应用档位 k=$k → waybar ${wb_px}px/${wb_h}px、fuzzel ${fz}pt、foot ${foot_px}px、光标 ${cursor}px"
    set_gtk "$k"
    set_foot "$foot_px"
    set_fuzzel "$fz" "$(calc "$BASE_FZ_HPAD" "$k")" "$(calc "$BASE_FZ_VPAD" "$k")" \
        "$(calc "$BASE_FZ_INNER" "$k")" "$(calc "$BASE_FZ_LINEH" "$k")"
    set_waybar "$wb_px" "$wb_h"
    set_cursor "$cursor"
    reload_desktop
    echo
    log "已生效：GTK 字号 ×$k（新开的 GTK 程序立即生效）"
    log "退出并重开 foot / fuzzel 才会用新字号；waybar 已重启"
}

status() {
    local scale px h fz foot_px cursor k
    echo "================ 字号 / 清晰度自检 ================"
    k="$(gsettings get org.gnome.desktop.interface text-scaling-factor 2>/dev/null || echo '?')"
    echo "1) GTK 字号缩放         : $k（1.0 = 原始）"
    echo "   GTK 字体             : $(gsettings get org.gnome.desktop.interface font-name 2>/dev/null || echo '?')"
    px="$(grep -oE 'font-size:\s*[0-9.]+px' "$WAYBAR_CSS" 2>/dev/null | head -1 | grep -oE '[0-9.]+' || true)"
    h="$(grep -oE '"height":\s*[0-9]+' "$WAYBAR_JSON" 2>/dev/null | head -1 | grep -oE '[0-9]+' || true)"
    echo "2) waybar              : font ${px:-?}px / height ${h:-?}px"
    fz="$(grep -oE '^font=.*size=[0-9]+' "$FUZZEL_INI" 2>/dev/null | grep -oE '[0-9]+$' || true)"
    echo "3) fuzzel              : ${fz:-?}pt（$(basename "$FUZZEL_INI")）"
    foot_px="$(grep -oE 'pixelsize=[0-9]+' "$FOOT_INI" 2>/dev/null | grep -oE '[0-9]+' || true)"
    echo "4) foot                : ${foot_px:-无配置（默认字号）}px"
    cursor="$(grep -oE '^Environment=XCURSOR_SIZE=[0-9]+' "$HOME_CFG/systemd/user/ww-session.service" 2>/dev/null | grep -oE '[0-9]+$' || true)"
    echo "5) 光标                : ${cursor:-默认 24}px"

    echo "--------------------------------------------------"
    echo "6) 当前输出缩放         : $(sig="$(ls -dt "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/hypr/*/ 2>/dev/null | head -1 | xargs -r basename || true)"; HYPRLAND_INSTANCE_SIGNATURE="$sig" hyprctl -j monitors 2>/dev/null | python3 -c 'import json,sys; m=json.load(sys.stdin)[0]; print("%s %dx%d scale=%s" % (m["name"], m["width"], m["height"], m["scale"]))' 2>/dev/null || echo '读不到')"
    echo "7) 整屏缩放变量（应为空）:"
    local v found=0
    for v in GDK_SCALE GDK_DPI_SCALE QT_SCALE_FACTOR QT_SCREEN_SCALE_FACTORS QT_AUTO_SCREEN_SCALE_FACTOR; do
        local cur
        cur="$(systemctl --user show-environment 2>/dev/null | grep -E "^$v=" || true)"
        if [ -n "$cur" ]; then echo "     $cur ← 这是糊源，unset 掉"; found=1; fi
    done
    [ "$found" = 0 ] && echo "     （无）"
    local cdsf
    cdsf="$(grep -oh 'force-device-scale-factor=[0-9.]*' "$HOME/.local/share/applications/google-chrome.desktop" 2>/dev/null | head -1 || true)"
    echo "8) Chrome 设备缩放      : ${cdsf:-未覆盖桌面项（默认 1x）}"
    echo "   网页缩放             : Chrome → 设置 → 外观 → 页面缩放（建议 125%，profile 里持久化）"
    echo "--------------------------------------------------"
    echo "判据：第 6 行必须是 scale=1.0，第 7 行必须为空；然后字号可以随便加大，画面都不会糊。"
}

case "${1:-}" in
    ""|apply) apply "${2:-1.3}" ;;
    status)  status ;;
    [0-9]*)  apply "$1" ;;
    *)
        echo "用法: $0 [1.0|1.15|1.3|1.45] | status" >&2; exit 1 ;;
esac
