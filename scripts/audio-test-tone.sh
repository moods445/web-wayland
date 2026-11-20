#!/usr/bin/env bash
# 在服务端默认 sink 上播一段 1kHz 正弦，用来验证「采集 → 网关 → 浏览器播放」整条音频链路。
# 用法: bash scripts/audio-test-tone.sh [秒数] [频率]
set -euo pipefail

sec="${1:-5}"
freq="${2:-1000}"

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "[tone] 需要 ffmpeg" >&2
    exit 1
fi

tmp="$(mktemp /tmp/ww-tone-XXXX.wav)"
trap 'rm -f "$tmp"' EXIT

ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i "sine=frequency=${freq}:sample_rate=48000:duration=${sec}" \
    -ac 2 -c:a pcm_s16le "$tmp"

echo "[tone] 播放 ${freq}Hz ${sec}s（浏览器端应能听到；若开了「静音目标机」则先取消勾选）"

if command -v pw-cat >/dev/null 2>&1; then
    pw-cat --playback "$tmp"
elif command -v pw-play >/dev/null 2>&1; then
    pw-play "$tmp"
else
    echo "[tone] 缺 pw-cat / pw-play（PipeWire 客户端）" >&2
    exit 1
fi

echo "[tone] 播放结束"
