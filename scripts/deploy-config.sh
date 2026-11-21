#!/usr/bin/env bash
# 把项目内配置部署到用户目录，幂等且带备份
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ASSETS="$ROOT/assets"
STAMP="$(date +%Y%m%d-%H%M%S)"

log() { printf '\033[1;34m[deploy]\033[0m %s\n' "$*"; }

backup() {
    local target="$1"
    if [ -e "$target" ] && [ ! -L "$target" ]; then
        mv "$target" "$target.bak.$STAMP"
        log "已备份 $target -> $target.bak.$STAMP"
    fi
}

mkdir -p "$ASSETS"

# 壁纸：没有就生成一张与新配色一致的深色渐变
if [ ! -f "$ASSETS/wallpaper.png" ]; then
    log "生成默认壁纸"
    ffmpeg -hide_banner -loglevel error -f lavfi \
        -i "gradients=s=3840x2160:c0=0x0b0b0f:c1=0x121218:c2=0x1a1a22:n=3:d=1" \
        -frames:v 1 -y "$ASSETS/wallpaper.png"
fi

# hypr
mkdir -p "$HOME/.config/hypr"
# 只备份要覆盖的文件：整目录 mv 会把 monitors.conf（用户档位）和 display.state 一起搬走
backup "$HOME/.config/hypr/hyprland.conf"
backup "$HOME/.config/hypr/hyprpaper.conf"
cp "$ROOT/config/hypr/hyprland.conf" "$HOME/.config/hypr/hyprland.conf"
# 壁纸路径注入（conf 里用占位符，避免硬编码机器路径）
sed -i "s|@WALLPAPER@|$ASSETS/wallpaper.png|g" "$HOME/.config/hypr/hyprland.conf"
cat > "$HOME/.config/hypr/hyprpaper.conf" <<EOF
preload = $ASSETS/wallpaper.png
wallpaper = eDP-1,$ASSETS/wallpaper.png
splash = false
EOF
# monitors.conf 是用户档位（分辨率/缩放/位置）：没有才写默认值，已存在不覆盖
if [ ! -f "$HOME/.config/hypr/monitors.conf" ] && [ -f "$ROOT/config/hypr/monitors.conf" ]; then
    cp "$ROOT/config/hypr/monitors.conf" "$HOME/.config/hypr/monitors.conf"
    log "已写入默认显示器档位：1920x1080@60 scale=1（逻辑 1920x1080，字号靠 font-scale.sh）"
fi
log "hypr 配置已部署"

# waybar
backup "$HOME/.config/waybar"
mkdir -p "$HOME/.config/waybar"
cp "$ROOT/config/waybar/config.jsonc" "$HOME/.config/waybar/config.jsonc"
cp "$ROOT/config/waybar/style.css" "$HOME/.config/waybar/style.css"
log "waybar 配置已部署"

# fuzzel
mkdir -p "$HOME/.config/fuzzel"
cp "$ROOT/config/fuzzel/fuzzel.ini" "$HOME/.config/fuzzel/fuzzel.ini"
log "fuzzel 配置已部署"

# foot（终端字号：pixelsize + dpi-aware=no，见 config/foot/foot.ini）
backup "$HOME/.config/foot/foot.ini"
mkdir -p "$HOME/.config/foot"
cp "$ROOT/config/foot/foot.ini" "$HOME/.config/foot/foot.ini"
log "foot 配置已部署"

# GTK 字体渲染（抗锯齿/微调）：已有 settings.ini 时不覆盖，只提示由 font-scale.sh 合并键值
for v in 3.0 4.0; do
    mkdir -p "$HOME/.config/gtk-$v"
    if [ ! -f "$HOME/.config/gtk-$v/settings.ini" ]; then
        cp "$ROOT/config/gtk-$v/settings.ini" "$HOME/.config/gtk-$v/settings.ini"
        log "gtk-$v settings.ini 已部署"
    else
        log "gtk-$v settings.ini 已存在，保留（抗锯齿键由 scripts/font-scale.sh 合并）"
    fi
done

# 字号档位：默认 1.3（只放大字号，不做任何画面缩放；1.0 可回滚）
if [ "${1:-}" != "--no-font" ] && [ -x "$ROOT/scripts/font-scale.sh" ]; then
    "$ROOT/scripts/font-scale.sh" 1.3 || log "font-scale.sh 执行失败，可稍后手动跑一次"
fi

# Chrome：Wayland 原生 + 固定 1x 设备缩放（分数 DSF 会让整窗先放大再降采样 → 发糊）。
# 网页内容用「页面缩放 125%」放大（文字按缩放后尺寸重绘，仍然清晰）。
# 用用户级 .desktop 覆盖（同名文件优先于 /usr/share/applications），不需要 root。
if command -v google-chrome-stable >/dev/null 2>&1 || [ -x /usr/bin/google-chrome-stable ]; then
    mkdir -p "$HOME/.local/share/applications"
    for d in google-chrome.desktop com.google.Chrome.desktop; do
        src="/usr/share/applications/$d"
        [ -f "$src" ] || continue
        sed -E 's|^Exec=/usr/bin/google-chrome-stable(.*)$|Exec=/usr/bin/google-chrome-stable --ozone-platform=wayland --force-device-scale-factor=1\1|' \
            "$src" > "$HOME/.local/share/applications/$d"
        log "Chrome 桌面项已覆盖：$d（Wayland 原生、DSF=1）"
    done
    update-desktop-database "$HOME/.local/share/applications" >/dev/null 2>&1 || true
    log "网页字号：Chrome → 设置 → 外观 → 页面缩放 125%（一次性，之后所有网站生效）"
fi

log "完成。重启会话：systemctl --user restart ww-session，或 hyprctl reload"
