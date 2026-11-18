#!/usr/bin/env bash
# 生成本地 CA 与服务器证书（SAN 含局域网 IP），供 HTTPS + QUIC 共用
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIR="$ROOT/certs"
IP="${1:-192.168.0.109}"
mkdir -p "$DIR"

if [ ! -f "$DIR/ca.key" ]; then
    echo "生成 CA"
    openssl ecparam -genkey -name prime256v1 -out "$DIR/ca.key" 2>/dev/null
    openssl req -x509 -new -key "$DIR/ca.key" -sha256 -days 3650 \
        -subj "/CN=web-wayland Local CA" -out "$DIR/ca.crt"
fi

echo "生成服务器证书（SAN: $IP）"
openssl ecparam -genkey -name prime256v1 -out "$DIR/server.key" 2>/dev/null
openssl req -new -key "$DIR/server.key" -subj "/CN=$IP" -out "$DIR/server.csr"
cat > "$DIR/san.cnf" <<EOF
subjectAltName=IP:$IP,IP:127.0.0.1,DNS:localhost
extendedKeyUsage=serverAuth
EOF
openssl x509 -req -in "$DIR/server.csr" -CA "$DIR/ca.crt" -CAkey "$DIR/ca.key" \
    -CAcreateserial -out "$DIR/server.crt" -days 825 -sha256 -extfile "$DIR/san.cnf" 2>/dev/null
rm -f "$DIR/server.csr"

# WebTransport 专用：自签、ECDSA P-256、有效期 <= 14 天（Chrome 哈希钉扎的硬性要求）
echo "生成 WebTransport 自签证书（13 天）"
openssl ecparam -genkey -name prime256v1 -out "$DIR/wt.key" 2>/dev/null
openssl req -x509 -new -key "$DIR/wt.key" -sha256 -days 13 -subj "/CN=$IP" \
    -addext "subjectAltName=IP:$IP,IP:127.0.0.1,DNS:localhost" -out "$DIR/wt.crt" 2>/dev/null
HASH="$(openssl x509 -in "$DIR/wt.crt" -outform der | openssl dgst -sha256 -binary | base64)"
printf '{"hash":"%s"}\n' "$HASH" > "$ROOT/web/cert-hash.json"
echo "哈希已写入 web/cert-hash.json: $HASH"

echo
echo "完成: $DIR/server.crt  $DIR/server.key  $DIR/ca.crt  $DIR/wt.crt"
echo
echo "在 macOS 上信任一次（只需一次）："
echo "  1) scp $DIR/ca.crt 到 Mac"
echo "  2) 双击导入「钥匙串访问 → 系统」，找到 'web-wayland Local CA' 改为「始终信任」"
echo "  3) 重启 Chrome 后访问 https://$IP:8443"
