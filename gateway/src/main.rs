// wwgw —— WebTransport 网关
//
// 职责：
//   1. HTTPS(8443/tcp) 托管前端静态文件
//   2. WebTransport(8443/udp) 把 wwcap 的 H.264 分片以不可靠 datagram 广播给客户端
//   3. 把客户端控制消息（键鼠/遥测）经 Unix DGRAM 转给 wwcap
//
// 数据来源：wwcap 通过 SOCK_DGRAM 向 /run/user/<uid>/ww-video.sock 发送完整分片（头+载荷）

use std::net::IpAddr;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;

/* 供 /healthz 与自动化测试断言的全局计数 */
static DATAGRAMS_IN: AtomicU64 = AtomicU64::new(0);
static DATAGRAMS_OUT: AtomicU64 = AtomicU64::new(0);
static SESSIONS: AtomicU64 = AtomicU64::new(0);
static ACTIVE: AtomicU64 = AtomicU64::new(0);

use anyhow::{Context, Result};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, UnixDatagram};
use tokio::sync::broadcast;
use tokio_rustls::TlsAcceptor;
use wtransport::{Endpoint, Identity, ServerConfig};

const PORT: u16 = 8443;

fn runtime_dir() -> PathBuf {
    std::env::var("XDG_RUNTIME_DIR").map(PathBuf::from).unwrap_or_else(|_| PathBuf::from("/tmp"))
}

fn ip_allowed(ip: IpAddr) -> bool {
    match ip {
        IpAddr::V4(v4) => {
            let o = v4.octets();
            o[0] == 192 && o[1] == 168 && o[2] == 0 // 192.168.0.0/24
                || v4.is_loopback()
        }
        IpAddr::V6(v6) => {
            if v6.is_loopback() {
                return true;
            }
            // IPv4-mapped（::ffff:a.b.c.d）要走 IPv4 规则
            match v6.to_ipv4_mapped() {
                Some(v4) => ip_allowed(IpAddr::V4(v4)),
                None => false,
            }
        }
    }
}

/* ── HTTPS 静态托管 ─────────────────────────────────────────── */
async fn serve_https(acceptor: TlsAcceptor, root: PathBuf) -> Result<()> {
    let listener = TcpListener::bind(("0.0.0.0", PORT)).await?;
    eprintln!("[gw] HTTPS 监听 :{PORT}  静态目录 {}", root.display());

    loop {
        let (stream, peer) = listener.accept().await?;
        if !ip_allowed(peer.ip()) {
            eprintln!("[gw] 拒绝非白名单来源 {peer}");
            continue;
        }
        let acceptor = acceptor.clone();
        let root = root.clone();
        tokio::spawn(async move {
            let mut tls = match acceptor.accept(stream).await {
                Ok(t) => t,
                Err(e) => {
                    eprintln!("[gw] TLS 握手失败 {peer}: {e}");
                    return;
                }
            };
            let mut buf = vec![0u8; 4096];
            let n = match tls.read(&mut buf).await {
                Ok(n) if n > 0 => n,
                _ => return,
            };
            let req = String::from_utf8_lossy(&buf[..n]);
            let raw = req
                .lines()
                .next()
                .and_then(|l| l.split_whitespace().nth(1))
                .unwrap_or("/");
            // 剥掉查询串，否则 /?auto=1 会被当成文件名
            let raw = raw.split('?').next().unwrap_or("/");

            // 内置健康检查：自动化测试可直接断言链路活性
            if raw == "/healthz" {
                let body = format!(
                    "{{\"ok\":true,\"sessions\":{},\"datagrams_in\":{},\"datagrams_out\":{}}}",
                    SESSIONS.load(Ordering::Relaxed),
                    DATAGRAMS_IN.load(Ordering::Relaxed),
                    DATAGRAMS_OUT.load(Ordering::Relaxed)
                );
                let head = format!(
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                    body.len()
                );
                let _ = tls.write_all(head.as_bytes()).await;
                let _ = tls.write_all(body.as_bytes()).await;
                let _ = tls.flush().await;
                return;
            }

            let path = if raw == "/" { "/index.html" } else { raw };
            // 防目录穿越
            let rel = path.trim_start_matches('/').replace("..", "");
            let file = root.join(rel);

            let (status, ctype, body) = match tokio::fs::read(&file).await {
                Ok(b) => {
                    let ct = match file.extension().and_then(|e| e.to_str()) {
                        Some("html") => "text/html; charset=utf-8",
                        Some("js") | Some("mjs") => "text/javascript; charset=utf-8",
                        Some("css") => "text/css; charset=utf-8",
                        Some("json") => "application/json",
                        Some("woff2") => "font/woff2",
                        _ => "application/octet-stream",
                    };
                    ("200 OK", ct, b)
                }
                Err(_) => ("404 Not Found", "text/plain", b"not found".to_vec()),
            };

            let head = format!(
                "HTTP/1.1 {status}\r\nContent-Type: {ctype}\r\nContent-Length: {}\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                body.len()
            );
            let _ = tls.write_all(head.as_bytes()).await;
            let _ = tls.write_all(&body).await;
            let _ = tls.flush().await;
        });
    }
}

#[tokio::main]
async fn main() -> Result<()> {
    // rustls 0.23 需要显式选择加密后端
    let _ = rustls::crypto::ring::default_provider().install_default();

    let rt = runtime_dir();
    let video_path = rt.join("ww-video.sock");
    let ctrl_path = rt.join("wwcap-ctrl.sock");

    let cert = std::env::var("WW_CERT").unwrap_or_else(|_| "certs/server.crt".into());
    let key = std::env::var("WW_KEY").unwrap_or_else(|_| "certs/server.key".into());
    let webroot = std::env::var("WW_WEBROOT").unwrap_or_else(|_| "web".into());

    /* TLS 证书：HTTPS 与 QUIC 共用 */
    let certs = {
        let data = std::fs::read(&cert).with_context(|| format!("读证书 {cert} 失败"))?;
        let mut rd = std::io::BufReader::new(&data[..]);
        rustls_pemfile::certs(&mut rd).collect::<Result<Vec<_>, _>>()?
    };
    let key_der = {
        let data = std::fs::read(&key).with_context(|| format!("读私钥 {key} 失败"))?;
        let mut rd = std::io::BufReader::new(&data[..]);
        rustls_pemfile::private_key(&mut rd)?.context("私钥为空")?
    };

    let mut tls_cfg = rustls::ServerConfig::builder()
        .with_no_client_auth()
        .with_single_cert(certs.clone(), key_der.clone_key())?;
    /* 静态托管只提供 HTTP/1.1（WebTransport 走独立的 QUIC/HTTP3 端点） */
    tls_cfg.alpn_protocols = vec![b"http/1.1".to_vec()];
    let acceptor = TlsAcceptor::from(Arc::new(tls_cfg));

    let root = std::fs::canonicalize(&webroot).unwrap_or_else(|_| PathBuf::from(&webroot));

    /* wwcap → 网关 的视频分片 */
    /* 上次异常退出可能留下 socket 文件 */
    let _ = std::fs::remove_file(&video_path);
    let video_sock = UnixDatagram::bind(&video_path)
        .with_context(|| format!("绑定 {} 失败", video_path.display()))?;
    let (tx, _rx) = broadcast::channel::<Arc<Vec<u8>>>(512);

    {
        let tx = tx.clone();
        tokio::spawn(async move {
            let mut buf = vec![0u8; 2048];
            let mut n_rx: u64 = 0;
            loop {
                match video_sock.recv(&mut buf).await {
                    Ok(n) if n > 0 => {
                        n_rx += 1;
                        DATAGRAMS_IN.fetch_add(1, Ordering::Relaxed);
                        if n_rx % 300 == 0 {
                            eprintln!("[gw] 已从 wwcap 收到 {n_rx} 个分片（最近 {n} 字节）");
                        }
                        let _ = tx.send(Arc::new(buf[..n].to_vec()));
                    }
                    _ => continue,
                }
            }
        });
    }

    /* 网关 → wwcap 的控制消息（send_to 只需 &self，用 Arc 共享即可） */
    let ctrl_sock = Arc::new(UnixDatagram::unbound()?);
    let ctrl_addr = ctrl_path.clone();

    tokio::spawn(serve_https(acceptor, root));

    /* WebTransport 端点 */
    /* WebTransport 用 13 天自签 ECDSA 证书，配合前端 serverCertificateHashes 钉扎 */
    let wt_cert = std::env::var("WW_WT_CERT").unwrap_or_else(|_| "certs/wt.crt".into());
    let wt_key = std::env::var("WW_WT_KEY").unwrap_or_else(|_| "certs/wt.key".into());
    let identity = Identity::load_pemfiles(&wt_cert, &wt_key)
        .await
        .with_context(|| format!("加载 WebTransport 证书 {wt_cert} 失败"))?;
    let config = ServerConfig::builder()
        .with_bind_default(PORT)
        .with_identity(identity)
        .build();
    let endpoint = Endpoint::server(config)?;
    eprintln!("[gw] WebTransport 监听 :{PORT}/udp");

    loop {
        let incoming = endpoint.accept().await;
        let tx = tx.clone();
        let ctrl_sock = ctrl_sock.clone();
        let ctrl_addr = ctrl_addr.clone();

        tokio::spawn(async move {
            let request = match incoming.await {
                Ok(r) => r,
                Err(e) => {
                    eprintln!("[gw] 会话请求失败: {e}");
                    return;
                }
            };
            let authority = request.authority().to_string();
            let connection = match request.accept().await {
                Ok(c) => c,
                Err(e) => {
                    eprintln!("[gw] 会话接受失败: {e}");
                    return;
                }
            };
            let peer = connection.remote_address();
            if !ip_allowed(peer.ip()) {
                eprintln!("[gw] 拒绝 {peer}（来源 {authority}）");
                return;
            }
            SESSIONS.fetch_add(1, Ordering::Relaxed);
            ACTIVE.fetch_add(1, Ordering::Relaxed);
            eprintln!("[gw] 会话建立 {peer}");

            /* 新客户端必须先拿到 IDR：请求 wwcap 强制关键帧 */
            let _ = ctrl_sock.send_to(b"{\"t\":\"kf\"}", &ctrl_addr).await;

            /* 下行：广播视频分片 */
            let mut rx = tx.subscribe();
            let conn_tx = connection.clone();
            let kf_sock = ctrl_sock.clone();
            let kf_addr = ctrl_addr.clone();
            let mut down = tokio::spawn(async move {
                let mut n_tx: u64 = 0;
                loop {
                    match rx.recv().await {
                        Ok(data) => {
                            if let Err(e) = conn_tx.send_datagram(data.as_slice()) {
                                eprintln!("[gw] send_datagram 失败: {e}");
                                break;
                            }
                            n_tx += 1;
                            DATAGRAMS_OUT.fetch_add(1, Ordering::Relaxed);
                            if n_tx % 300 == 0 {
                                eprintln!("[gw] 已向客户端发出 {n_tx} 个分片");
                            }
                        }
                        Err(broadcast::error::RecvError::Lagged(n)) => {
                            /* 丢片后必须尽快补一个 IDR，否则要等下一个 GOP */
                            eprintln!("[gw] 客户端落后 {n} 个分片，请求关键帧");
                            let _ = kf_sock.send_to(b"{\"t\":\"kf\"}", &kf_addr).await;
                        }
                        Err(_) => break,
                    }
                }
            });

            /* 上行：控制流 */
            let up = async {
                let (mut send, mut recv) = connection.accept_bi().await.ok()?;
                let mut acc = Vec::new();
                let mut buf = [0u8; 4096];
                loop {
                    let n = recv.read(&mut buf).await.ok()??;
                    if n == 0 {
                        break;
                    }
                    acc.extend_from_slice(&buf[..n]);
                    while acc.len() >= 4 {
                        let len = u32::from_le_bytes([acc[0], acc[1], acc[2], acc[3]]) as usize;
                        if len > 64 * 1024 {
                            eprintln!("[gw] 控制消息过大（{len}），断开该会话");
                            return None;
                        }
                        if acc.len() < 4 + len {
                            break;
                        }
                        let msg = acc[4..4 + len].to_vec();
                        acc.drain(..4 + len);
                        let _ = ctrl_sock.send_to(&msg, &ctrl_addr).await;
                        /* 回执：把遥测回显，便于客户端计算 RTT */
                        let out = b"{\"t\":\"ack\"}";
                        let mut frame = (out.len() as u32).to_le_bytes().to_vec();
                        frame.extend_from_slice(out);
                        let _ = send.write_all(&frame).await;
                    }
                }
                let _ = send.finish().await;
                Some(())
            };

            tokio::select! {
                _ = &mut down => {},
                _ = up => {},
            }
            /* 另一侧必须显式 abort：select 只是 detach，不 abort 会泄漏一个永久任务 */
            down.abort();
            /* 最后一个客户端离开：让桌面回到默认 1080p，别停在小窗口尺寸上 */
            if ACTIVE.fetch_sub(1, Ordering::Relaxed) <= 1 {
                let _ = ctrl_sock.send_to(b"{\"t\":\"mode\",\"reset\":1}", &ctrl_addr).await;
            }
            eprintln!("[gw] 会话结束 {peer}");
        });
    }
}
