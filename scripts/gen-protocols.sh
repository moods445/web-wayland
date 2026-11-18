#!/usr/bin/env bash
# 从 wayland-protocols 生成 C 绑定（幂等）
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GEN="$ROOT/capture/src/generated"
WP="$(pkg-config --variable=pkgdatadir wayland-protocols)"

mkdir -p "$GEN"

gen() {
    local xml="$1" name="$2"
    wayland-scanner private-code "$xml" "$GEN/$name.c"
    wayland-scanner client-header "$xml" "$GEN/$name.h"
    echo "generated $name"
}

gen "$WP/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml" ext-image-copy-capture-v1
gen "$WP/staging/ext-image-capture-source/ext-image-capture-source-v1.xml" ext-image-capture-source-v1
gen "$WP/stable/linux-dmabuf/linux-dmabuf-v1.xml" linux-dmabuf-v1
gen "$WP/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml" ext-foreign-toplevel-list-v1
gen "$ROOT/proto/virtual-keyboard-unstable-v1.xml" virtual-keyboard-unstable-v1
gen "$ROOT/proto/wlr-virtual-pointer-unstable-v1.xml" wlr-virtual-pointer-unstable-v1
gen "$ROOT/proto/wlr-screencopy-unstable-v1.xml" wlr-screencopy-unstable-v1
gen "$WP/stable/viewporter/viewporter.xml" viewporter
