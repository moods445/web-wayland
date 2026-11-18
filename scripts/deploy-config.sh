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
mkdir -p "$HOME/.config"
backup "$HOME/.config/hypr"
mkdir -p "$HOME/.config/hypr"
cp "$ROOT/config/hypr/hyprland.conf" "$HOME/.config/hypr/hyprland.conf"
# 壁纸路径注入（conf 里用占位符，避免硬编码机器路径）
sed -i "s|@WALLPAPER@|$ASSETS/wallpaper.png|g" "$HOME/.config/hypr/hyprland.conf"
cat > "$HOME/.config/hypr/hyprpaper.conf" <<EOF
preload = $ASSETS/wallpaper.png
wallpaper = eDP-1,$ASSETS/wallpaper.png
splash = false
EOF
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

log "完成。重启会话：pkill Hyprland 后重新拉起，或用 hyprctl reload"
