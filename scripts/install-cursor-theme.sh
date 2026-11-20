#!/usr/bin/env bash
# 安装「全透明光标」主题 ww-invisible。
#
# 用途：让合成器不把光标画进视频帧，改由客户端自绘（延迟 0ms）。
# 注意：Hyprland 运行时不会重新加载 Xcursor 主题——`hyprctl setcursor` 是空操作，
#       必须通过 XCURSOR_THEME 环境变量在**启动时**加载（见 scripts/ww-session.service），
#       并且 config/hypr/hyprland.conf 里要 `cursor:enable_hyprcursor = false`。
#
# 用法：
#   ./scripts/install-cursor-theme.sh          # 安装/重建到 ~/.icons/ww-invisible
#   ./scripts/install-cursor-theme.sh --check  # 只检查是否已安装
set -euo pipefail

THEME=ww-invisible
DEST="${HOME}/.icons/${THEME}"
NAMES=(
  left_ptr default arrow pointer hand hand1 hand2 text xterm crosshair cross edit
  pencil watch wait progress size_all move fleur all-scroll help question_arrow
  context-menu copy alias no-drop not-allowed
  e-resize w-resize n-resize s-resize ew-resize ns-resize nesw-resize nwse-resize
  col-resize row-resize grabbing grab zoom-in zoom-out
  sb_h_double_arrow sb_v_double_arrow top_left_corner
)

if [[ "${1:-}" == "--check" ]]; then
    if [[ -f "${DEST}/cursors/left_ptr" ]]; then
        echo "已安装: ${DEST}（${#NAMES[@]} 个光标名）"
        exit 0
    fi
    echo "未安装: ${DEST}（运行 ./scripts/install-cursor-theme.sh）" >&2
    exit 1
fi

command -v xcursorgen >/dev/null || { echo "缺少 xcursorgen（apt install xcursorgen）" >&2; exit 1; }
command -v python3 >/dev/null || { echo "缺少 python3" >&2; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# 24×24 全透明 RGBA PNG（用脚本生成，避免往仓库里塞二进制）
python3 - "$TMP/blank.png" <<'PY'
import struct, sys, zlib
w = h = 24
raw = b"".join(b"\x00" + b"\x00\x00\x00\x00" * w for _ in range(h))
def chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
png = (b"\x89PNG\r\n\x1a\n"
       + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
       + chunk(b"IDAT", zlib.compress(raw))
       + chunk(b"IEND", b""))
open(sys.argv[1], "wb").write(png)
PY

printf '24 0 0 %s\n' "$TMP/blank.png" > "$TMP/cursor.conf"

rm -rf "$DEST"
mkdir -p "$DEST/cursors"
for n in "${NAMES[@]}"; do
    xcursorgen "$TMP/cursor.conf" "$DEST/cursors/$n"
done
printf '[Icon Theme]\nName=%s\nComment=Transparent cursors (client draws its own)\n' "$THEME" > "$DEST/index.theme"

echo "已安装: $DEST（${#NAMES[@]} 个光标名）"
echo "下一步：重启桌面会话，让 XCURSOR_THEME 生效"
echo "  systemctl --user restart ww-session ww-gateway ww-cap"
echo "验证：指针移到两个位置各截一帧，差异像素应为 0"
echo "  grim -o HEADLESS-2 /tmp/a.png; grim -o HEADLESS-2 /tmp/b.png"
