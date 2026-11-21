#!/usr/bin/env bash
# 把本机切换成「只做 Moonlight + Sunshine 串流主机」的部署形态：
#
#   ww-session        Hyprland 桌面（保留：Sunshine 必须有合成器才能抓帧）
#   sunshine-wayland  Sunshine（用户单元，带正确的 Wayland 环境）
#
# 同时关闭浏览器链路与它们的开机自启：
#   ww-cap / ww-gateway  停用 + 取消自启
#   app-dev.lizardbyte.app.Sunshine.service（发行版系统单元）
#     在开机时以 DISPLAY=:0 / XDG_SESSION_TYPE=x11 启动，拿不到 WAYLAND_DISPLAY，
#     抓帧必定失败 → Moonlight 黑屏。停用并 mask 掉，改由用户单元接管。
#
# 用法：
#   scripts/install-sunshine-host.sh                 # 安装/更新，不动正在跑的会话
#   scripts/install-sunshine-host.sh --restart-session # 连 Hyprland 一起重启（应用新环境变量，桌面窗口会关掉）
#   scripts/install-sunshine-host.sh --status
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
DISTRO_UNIT=app-dev.lizardbyte.app.Sunshine.service
RETIRED=(ww-cap.service ww-gateway.service)
SESSION=ww-session.service
SUN=sunshine-wayland.service

log()  { printf '\033[1;34m[host]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[host]\033[0m %s\n' "$*"; }

status() {
    log "当前状态"
    local u a e
    for u in "$SESSION" "$SUN" "${RETIRED[@]}" "$DISTRO_UNIT"; do
        a="$(systemctl --user is-active "$u" 2>/dev/null | head -1 || true)"
        [ -n "$a" ] || a="$(systemctl is-active "$u" 2>/dev/null | head -1 || true)"
        e="$(systemctl --user is-enabled "$u" 2>/dev/null | head -1 || true)"
        [ -n "$e" ] || e="$(systemctl is-enabled "$u" 2>/dev/null | head -1 || true)"
        printf '  %-42s active=%-8s enabled=%s\n' "$u" "${a:-?}" "${e:-?}"
    done
    echo "  linger=$(loginctl show-user "$(id -un)" -p Linger 2>/dev/null | cut -d= -f2)"
    bash "$ROOT/scripts/sunshine-wayland.sh" status
}

# 发行版 Sunshine 单元的实际作用域：本机是 user 单元（/usr/lib/systemd/user/），
# 由 user manager 直接拉起，环境里只有 DISPLAY=:0 / XDG_SESSION_TYPE=x11。
distro_scope() {
    local ls
    ls="$(systemctl --user show -p LoadState --value "$DISTRO_UNIT" 2>/dev/null)"
    case "$ls" in loaded|masked) echo user; return ;; esac
    ls="$(systemctl show -p LoadState --value "$DISTRO_UNIT" 2>/dev/null)"
    case "$ls" in loaded|masked) echo system; return ;; esac
    echo none
}

case "${1:-}" in
    "") ;;
    --status|status) status; exit 0 ;;
    --restart-session) ;;
    *) echo "用法: $0 [--restart-session|--status]" >&2; exit 1 ;;
esac
RESTART_SESSION=0
[ "${1:-}" = "--restart-session" ] && RESTART_SESSION=1

# ── 1. 退休浏览器链路（先停，避免和 Sunshine 抢 screencopy / 8443 端口）──────
for u in "${RETIRED[@]}"; do
    if systemctl --user list-unit-files "$u" >/dev/null 2>&1; then
        systemctl --user disable --now "$u" 2>/dev/null || true
        log "已停用并取消自启：$u"
    fi
done

# ── 2. 关掉发行版自带的 Sunshine 单元（环境错 → 抓帧必失败 → 黑屏）──────────
# 本机是 user 单元：user manager 在开机（linger）时直接拉起，环境里只有
# DISPLAY=:0 / XDG_SESSION_TYPE=x11，没有 WAYLAND_DISPLAY。必须停用并 mask，
# 否则它和本项目的 sunshine-wayland 抢 47984-47990 端口。
case "$(distro_scope)" in
    user)
        systemctl --user disable --now "$DISTRO_UNIT" 2>/dev/null || true
        systemctl --user mask "$DISTRO_UNIT" >/dev/null 2>&1 || true
        log "已停用并 mask：$DISTRO_UNIT（user 单元）"
        ;;
    system)
        if sudo -n true 2>/dev/null; then
            sudo systemctl disable --now "$DISTRO_UNIT" 2>/dev/null || true
            sudo systemctl mask "$DISTRO_UNIT" >/dev/null 2>&1 || true
            log "已停用并 mask：$DISTRO_UNIT（system 单元）"
        else
            warn "需要 sudo 才能停/屏蔽 $DISTRO_UNIT，请手动执行："
            warn "  sudo systemctl disable --now $DISTRO_UNIT && sudo systemctl mask $DISTRO_UNIT"
        fi
        ;;
    *) warn "没找到发行版单元 $DISTRO_UNIT，跳过" ;;
esac

# ── 2.5 纯 Wayland：关掉 X11 相关的自启 ────────────────────────────────────
# user manager 里残留的 DISPLAY 会被**所有**用户单元继承（发行版 Sunshine 单元
# 「在 X11 环境里跑」就是这么来的），直接清掉。
systemctl --user unset-environment DISPLAY XAUTHORITY 2>/dev/null || true
log "已清掉 user manager 里的 DISPLAY / XAUTHORITY"

# LXQt 是 X11 桌面：/etc/xdg/autostart/lxqt-*.desktop 由 autostart 生成器变成
# app-lxqt-*@autostart.service，这里全部 mask，免得哪天切到 LXQt 又把 X11 拉起来。
for u in desktop globalkeyshortcuts notifications panel policykit-agent powermanagement runner; do
    systemctl --user mask "app-lxqt-${u}@autostart.service" >/dev/null 2>&1 || true
done
log "已 mask LXQt（X11）自启单元"

# 用户自启目录里的 X11 脚本（xset/xrandr 之类）挪走：只扫顶层 *.desktop，子目录不生效
if [ -f "$HOME/.config/autostart/screen-blank-1min.desktop" ]; then
    mkdir -p "$HOME/.config/autostart/disabled"
    mv -f "$HOME/.config/autostart/screen-blank-1min.desktop" "$HOME/.config/autostart/disabled/"
    log "已停用 X11 自启项 screen-blank-1min.desktop（xset dpms）"
fi

# X11 显示管理器 / x11-common：本机只跑 Hyprland + Xwayland=off，全部 mask
for u in sddm gdm3 lightdm xdm x11-common; do
    st="$(systemctl is-enabled "$u.service" 2>/dev/null | head -1 || true)"
    case "$st" in
        enabled|disabled)
            if sudo -n true 2>/dev/null; then
                sudo systemctl mask "$u.service" >/dev/null 2>&1 || true
                log "已 mask $u.service（X11）"
            else
                warn "需要 sudo 才能 mask $u.service（X11 自启）"
            fi
            ;;
    esac
done

# Hyprland 侧：Xwayland 必须关掉（config/hypr/hyprland.conf 里已写 false）
if grep -qE '^\s*enabled\s*=\s*true' <(sed -n '/^xwayland\s*{/,/}/p' "$HOME/.config/hypr/hyprland.conf" 2>/dev/null); then
    warn "~/.config/hypr/hyprland.conf 里 xwayland 仍是 enabled = true（改 false 后重启会话）"
fi

# ── 3. 安装用户单元 ────────────────────────────────────────────────────────
mkdir -p "$UNIT_DIR"
for u in "$SESSION" "$SUN"; do
    install -m 644 "$ROOT/scripts/$u" "$UNIT_DIR/$u"
    log "已安装 $UNIT_DIR/$u"
done
systemctl --user daemon-reload

# 无图形登录也要常驻：linger
loginctl enable-linger "$(id -un)" 2>/dev/null || true

systemctl --user enable "$SESSION" "$SUN" >/dev/null
log "已启用开机自启：$SESSION + $SUN"

# ── 4. 启动/重启 ──────────────────────────────────────────────────────────
if [ "$RESTART_SESSION" = 1 ]; then
    log "重启会话与 Sunshine（桌面里的窗口会被关掉）"
    systemctl --user restart "$SESSION"
    systemctl --user restart "$SUN"
elif systemctl --user is-active --quiet "$SESSION"; then
    log "会话已在运行，未重启；Sunshine 单独应用新配置"
    systemctl --user restart "$SUN"
else
    systemctl --user start "$SESSION"
    systemctl --user start "$SUN"
fi

sleep 2
status
if grep -q 'ww-invisible' "$UNIT_DIR/$SESSION" 2>/dev/null; then
    warn "$SESSION 里仍带着透明光标主题，串流会看不到指针：用 --restart-session 重启会话。"
fi
