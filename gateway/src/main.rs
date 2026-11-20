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
use std::time::Instant;

/* 供 /healthz 与自动化测试断言的全局计数 */
static DATAGRAMS_IN: AtomicU64 = AtomicU64::new(0);
static DATAGRAMS_OUT: AtomicU64 = AtomicU64::new(0);
/* 音频独立计数：音频是 ~50 包/秒的稳定流，和视频分片量纲不同，混在一起看不出问题 */
static AUDIO_IN: AtomicU64 = AtomicU64::new(0);
static AUDIO_OUT: AtomicU64 = AtomicU64::new(0);
static AUDIO_DROPPED: AtomicU64 = AtomicU64::new(0);
static SESSIONS: AtomicU64 = AtomicU64::new(0);
static ACTIVE: AtomicU64 = AtomicU64::new(0);
static NEXT_SESSION: AtomicU64 = AtomicU64::new(1);
/* /trace-upload 限速用：最近一次真正落盘的时刻（ms），避免多标签页把磁盘刷爆 */
static LAST_TRACE_UPLOAD_MS: AtomicU64 = AtomicU64::new(0);
/* 当前拥有输入注入权的会话：多客户端时避免两个指针互相拽动光标。
   INPUT_OWNER 只是「当前操作者」，不是永久的：owner 超过 INPUT_LEASE_MS
   没有输入就把控制权让给别的会话，否则一个没断干净的旧会话（例如异常退出
   的浏览器标签页、自动化测试）会永久占住鼠标，新用户怎么动都没反应。 */
static INPUT_OWNER: AtomicU64 = AtomicU64::new(0);
/* owner 最近一次发输入的时刻（ms，UNIX_EPOCH 起算） */
static INPUT_LAST: AtomicU64 = AtomicU64::new(0);
const INPUT_LEASE_MS: u64 = 1500;

/* 音频 RED 冗余档位（1=每包带上一帧）。客户端上报丢包率后调整，
   每隔 AUDIO_RED_HOLD_MS 最多改一次，避免在阈值附近来回跳。 */
static AUDIO_RED: AtomicU64 = AtomicU64::new(1);
static AUDIO_RED_AT: AtomicU64 = AtomicU64::new(0);
const AUDIO_RED_HOLD_MS: u64 = 5000;

/* 客户端主动重启流服务的节流：重启本身要几秒，太频繁只会让画面更糟 */
static RESTART_AT: AtomicU64 = AtomicU64::new(0);
const RESTART_HOLD_MS: u64 = 5000;
static RESTARTS: AtomicU64 = AtomicU64::new(0);

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

/* 从控制 JSON 里取出 {"t":"xx"} 的类型（够用即可，避免引 JSON 库） */
fn ctrl_type(msg: &[u8]) -> Option<&[u8]> {
    let s = std::str::from_utf8(msg).ok()?;
    let p = s.find("\"t\":\"")? + 5;
    let rest = &s[p..];
    let end = rest.find('"')?;
    Some(&msg[p..p + end])
}

fn is_input(t: &[u8]) -> bool {
    matches!(t, b"pm" | b"pr" | b"pb" | b"pw" | b"kk")
}

/* 往客户端控制流回一条 JSON（长度前缀与 web/src/proto.js 的 encodeCtrl 一致） */
async fn ctrl_reply(
    send: &mut wtransport::SendStream,
    json: &str,
) -> Result<(), wtransport::error::StreamWriteError> {
    let mut framed = Vec::with_capacity(4 + json.len());
    framed.extend_from_slice(&(json.len() as u32).to_le_bytes());
    framed.extend_from_slice(json.as_bytes());
    send.write_all(&framed).await
}

/* 取 JSON 里的数值字段（只有 astats 这种扁平消息需要，避免引 serde_json） */
fn json_num(msg: &[u8], key: &str) -> Option<f64> {
    let s = std::str::from_utf8(msg).ok()?;
    let pat = format!("\"{key}\":");
    let p = s.find(&pat)? + pat.len();
    s[p..]
        .trim_start()
        .split(|c: char| c == ',' || c == '}' || c.is_whitespace())
        .next()?
        .parse()
        .ok()
}

fn now_ms() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

/* 从首包缓冲区里取出 POST body：按 Content-Length 把剩余字节补齐。
   浏览器用字符串 body 的 fetch 一定带 Content-Length，这里不处理 chunked。 */
async fn read_request_body(
    tls: &mut tokio_rustls::server::TlsStream<tokio::net::TcpStream>,
    head: &[u8],
    max: usize,
) -> Option<Vec<u8>> {
    let text = String::from_utf8_lossy(head);
    let mut len: Option<usize> = None;
    for line in text.lines() {
        if line.is_empty() {
            break;
        }
        let lower = line.to_ascii_lowercase();
        if let Some(v) = lower.strip_prefix("content-length:") {
            len = v.trim().parse().ok();
        }
    }
    let cl = len?;
    if cl > max {
        return None;
    }
    let sep = head.windows(4).position(|w| w == b"\r\n\r\n")? + 4;
    let mut body: Vec<u8> = head.get(sep..).unwrap_or(&[]).to_vec();
    while body.len() < cl {
        let mut tmp = vec![0u8; 64 * 1024];
        match tls.read(&mut tmp).await {
            Ok(0) => break,
            Ok(k) => body.extend_from_slice(&tmp[..k]),
            Err(_) => break,
        }
    }
    if body.len() < cl {
        return None;
    }
    body.truncate(cl);
    Some(body)
}

/* ── HTTPS 静态托管 ─────────────────────────────────────────── */
async fn serve_https(acceptor: TlsAcceptor, root: PathBuf) -> Result<()> {
    let listener = TcpListener::bind(("0.0.0.0", PORT)).await?;
    eprintln!("[gw] HTTPS 监听 :{PORT}  静态目录 {}", root.display());

    loop {
        /* 单个 accept 的瞬时错误（EMFILE/ECONNABORTED）不能把整个服务带走 */
        let (stream, peer) = match listener.accept().await {
            Ok(v) => v,
            Err(e) => {
                eprintln!("[gw] accept 失败: {e}");
                tokio::time::sleep(std::time::Duration::from_millis(100)).await;
                continue;
            }
        };
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

            /* 前端自动上传的输入追踪（POST，NDJSON body）。用户不需要手动导出：
               每 15 秒一份滚动窗口，卡顿突刺时立刻补一份。
               文件是**追加**的（不覆盖），所以可以 tail -f / 逐行 jq：
                 * ww-client-trace.jsonl —— 一行一条记录（snap / ev / pair），
                   超过 WW_TRACE_MAX_BYTES（默认 2MB）轮转成 .jsonl.1，只保留一份；
                 * 服务端 wwcap-trace.jsonl —— 同样是追加行式，2MB 轮转。
               总计 ≤ 4MB。另做了上传限速（网关 1.5 秒内只接受一份），
               避免多标签页/异常重连把盘刷爆。 */
            if raw == "/trace-upload" {
                const MAX_BODY: usize = 1024 * 1024;   // 单份上限 1MB，正常约 150KB
                let body = match read_request_body(&mut tls, &buf[..n], MAX_BODY).await {
                    Some(b) if !b.is_empty() && b.iter().any(|c| !c.is_ascii_whitespace()) => b,
                    _ => {
                        let _ = tls
                            .write_all(b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
                            .await;
                        let _ = tls.flush().await;
                        return;
                    }
                };

                /* 限速：1.5 秒内重复上传直接丢弃（204，不写盘） */
                let now_ms = std::time::SystemTime::now()
                    .duration_since(std::time::UNIX_EPOCH)
                    .map(|d| d.as_millis() as u64)
                    .unwrap_or(0);
                let prev = LAST_TRACE_UPLOAD_MS.swap(now_ms, Ordering::Relaxed);
                let too_soon = now_ms.saturating_sub(prev) < 1500;

                let dir = std::env::var("WW_TRACE_DIR").unwrap_or_else(|_| "/tmp".to_string());
                let hist_max: u64 = std::env::var("WW_TRACE_MAX_BYTES")
                    .ok()
                    .and_then(|v| v.parse::<u64>().ok())
                    .filter(|v| *v >= 256 * 1024)
                    .unwrap_or(2 * 1024 * 1024);
                let hist = std::path::Path::new(&dir).join("ww-client-trace.jsonl");

                let mut ok = true;
                if !too_soon {
                    /* 超限先轮转：只保留 .jsonl 与 .jsonl.1 两份 */
                    if let Ok(md) = tokio::fs::metadata(&hist).await {
                        if md.len() + body.len() as u64 > hist_max {
                            let _ = tokio::fs::rename(&hist, hist.with_extension("jsonl.1")).await;
                        }
                    }
                    if let Ok(mut f) = tokio::fs::OpenOptions::new()
                        .create(true)
                        .append(true)
                        .open(&hist)
                        .await
                    {
                        use tokio::io::AsyncWriteExt as _;
                        ok = async {
                            f.write_all(&body).await?;
                            if !body.ends_with(b"\n") {
                                f.write_all(b"\n").await?;
                            }
                            f.flush().await
                        }
                        .await
                        .is_ok();
                    }
                }
                let resp = if ok {
                    "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
                } else {
                    "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
                };
                let _ = tls.write_all(resp.as_bytes()).await;
                let _ = tls.flush().await;
                return;
            }

            // 内置健康检查：自动化测试可直接断言链路活性
            if raw == "/healthz" {
                let body = format!(
                    "{{\"ok\":true,\"sessions_total\":{},\"active\":{},\"datagrams_in\":{},\"datagrams_out\":{},\
                      \"audio_in\":{},\"audio_out\":{},\"audio_dropped\":{},\"restarts\":{}}}",
                    SESSIONS.load(Ordering::Relaxed),
                    ACTIVE.load(Ordering::Relaxed),
                    DATAGRAMS_IN.load(Ordering::Relaxed),
                    DATAGRAMS_OUT.load(Ordering::Relaxed),
                    AUDIO_IN.load(Ordering::Relaxed),
                    AUDIO_OUT.load(Ordering::Relaxed),
                    AUDIO_DROPPED.load(Ordering::Relaxed),
                    RESTARTS.load(Ordering::Relaxed)
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

            // 服务端编码器能力：wwcap 启动/切换编码器时写入能力文件
            if raw == "/capabilities" {
                let caps_path = std::env::var("WW_CAPS_PATH")
                    .unwrap_or_else(|_| "/tmp/wwcap-caps.json".to_string());
                /* 文件不存在说明 wwcap 尚未就绪：返回 503，前端保持「未知」而不是误置灰 */
                let (status, body) = match tokio::fs::read_to_string(&caps_path).await {
                    Ok(b) => ("200 OK", b),
                    Err(_) => (
                        "503 Service Unavailable",
                        "{\"chroma444\":0,\"encoder\":\"\"}".to_string(),
                    ),
                };
                let head = format!(
                    "HTTP/1.1 {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                    status,
                    body.len()
                );
                let _ = tls.write_all(head.as_bytes()).await;
                let _ = tls.write_all(body.as_bytes()).await;
                let _ = tls.flush().await;
                return;
            }

            // 输入/画面追踪：wwcap 每 5 秒把新增记录追加进 .jsonl，这里直接读文件返回。
            // 只在白名单来源可访问（上面的 ip_allowed 已经拦过一次）。
            if raw == "/trace" {
                let trace_path = std::env::var("WW_TRACE_PATH")
                    .unwrap_or_else(|_| "/tmp/wwcap-trace.jsonl".to_string());
                let (status, body) = match tokio::fs::read_to_string(&trace_path).await {
                    Ok(b) => ("200 OK", b),
                    Err(_) => (
                        "503 Service Unavailable",
                        "{\"type\":\"error\",\"error\":\"还没有追踪数据（wwcap 刚起或没有输入）\"}\n"
                            .to_string(),
                    ),
                };
                let head = format!(
                    "HTTP/1.1 {}\r\nContent-Type: application/x-ndjson\r\nContent-Length: {}\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                    status,
                    body.len()
                );
                let _ = tls.write_all(head.as_bytes()).await;
                let _ = tls.write_all(body.as_bytes()).await;
                let _ = tls.flush().await;
                return;
            }

            let path = if raw == "/" { "/index.html" } else { raw };
            /* 防目录穿越：拒绝任何含 ".." 或空路径段的请求。
               旧实现 replace("..","") 会把 "/....//etc/passwd" 变成绝对路径。 */
            let rel = path.trim_start_matches('/');
            let invalid = rel.is_empty() || rel.split('/').any(|s| s.is_empty() || s == "." || s == "..");
            let file = root.join(if invalid { "__ww_invalid__" } else { rel });

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
    let audio_path = rt.join("ww-audio.sock");
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
    /* 512 个分片在 80 Mbps 下只有 ~57ms 缓冲，QUIC 写入稍有停顿就会 Lagged
       并触发关键帧风暴；2048 约等于 4 帧（1080p60），内存 ~2MB/订阅者。 */
    let (tx, _rx) = broadcast::channel::<Arc<Vec<u8>>>(2048);

    {
        let tx = tx.clone();
        tokio::spawn(async move {
            let mut buf = vec![0u8; 2048];
            let mut n_rx: u64 = 0;
            /* 按时间限流：80 Mbps 下每秒 ~9000 个分片，按计数打印会刷爆 journald */
            let mut next_log = Instant::now();
            loop {
                match video_sock.recv(&mut buf).await {
                    Ok(n) if n > 0 => {
                        n_rx += 1;
                        DATAGRAMS_IN.fetch_add(1, Ordering::Relaxed);
                        if Instant::now() >= next_log {
                            next_log = Instant::now() + std::time::Duration::from_secs(5);
                            eprintln!("[gw] 已从 wwcap 收到 {n_rx} 个分片（最近 {n} 字节）");
                        }
                        let _ = tx.send(Arc::new(buf[..n].to_vec()));
                    }
                    _ => continue,
                }
            }
        });
    }

    /* wwcap → 网关 的音频包（16B 头 + Opus 主帧 + 可选冗余帧） */
    /* 音频单独一条 socket + 独立 broadcast：视频一帧上千个分片，
       混在同一条通道里会让音频排在视频后面，延迟不可控。 */
    let _ = std::fs::remove_file(&audio_path);
    let audio_sock = UnixDatagram::bind(&audio_path)
        .with_context(|| format!("绑定 {} 失败", audio_path.display()))?;
    let (tx_audio, _rx_audio) = broadcast::channel::<Arc<Vec<u8>>>(256);
    {
        let tx = tx_audio.clone();
        tokio::spawn(async move {
            let mut buf = vec![0u8; 2048];
            let mut n_rx: u64 = 0;
            loop {
                match audio_sock.recv(&mut buf).await {
                    Ok(n) if n > 0 => {
                        n_rx += 1;
                        AUDIO_IN.fetch_add(1, Ordering::Relaxed);
                        if n_rx % 2500 == 0 {
                            eprintln!("[gw] 已从 wwcap 收到 {n_rx} 个音频包（最近 {n} 字节）");
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
        let tx_audio = tx_audio.clone();
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
            let sid = NEXT_SESSION.fetch_add(1, Ordering::Relaxed);
            eprintln!("[gw] 会话建立 {peer}（#{sid}）");

            /* 新客户端必须先拿到 IDR：请求 wwcap 强制关键帧 */
            let _ = ctrl_sock.send_to(b"{\"t\":\"kf\"}", &ctrl_addr).await;
            /* 有人看才采音：wwcap 收到 rx=1 才启动 PipeWire 采集与 Opus 编码 */
            let _ = ctrl_sock
                .send_to(b"{\"t\":\"astream\",\"rx\":1}", &ctrl_addr)
                .await;
            /* 会话存续：wwcap 空闲 30 秒会休眠（停抓帧/编码），有人连上立即唤醒 */
            let _ = ctrl_sock
                .send_to(b"{\"t\":\"active\",\"v\":1}", &ctrl_addr)
                .await;

            /* 下行：广播视频分片 */
            let mut rx = tx.subscribe();
            let conn_tx = connection.clone();
            let kf_sock = ctrl_sock.clone();
            let kf_addr = ctrl_addr.clone();
            let mut down = tokio::spawn(async move {
                let mut n_tx: u64 = 0;
                let mut errs: u32 = 0;
                let mut next_log = Instant::now() + std::time::Duration::from_secs(5);
                loop {
                    match rx.recv().await {
                        Ok(data) => {
                            /* datagram 失败分两种：路径 MTU 变小（TooLarge）不该断流，
                               网络断开（NotConnected）最终要靠连续失败阈值收尾。
                               以前一有错误就 break，会让客户端永久定格在最后一帧。 */
                            if let Err(e) = conn_tx.send_datagram(data.as_slice()) {
                                errs += 1;
                                if errs == 1 || errs % 100 == 0 {
                                    eprintln!("[gw] send_datagram 失败（第 {errs} 次，跳过该分片）: {e}");
                                }
                                if errs > 200 {
                                    break;
                                }
                                continue;
                            }
                            errs = 0;
                            n_tx += 1;
                            DATAGRAMS_OUT.fetch_add(1, Ordering::Relaxed);
                            if Instant::now() >= next_log {
                                next_log = Instant::now() + std::time::Duration::from_secs(5);
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

            /* 下行：音频包。和视频共用一个 connection，前端按首字节分流 */
            let mut rx_audio = tx_audio.subscribe();
            let conn_audio = connection.clone();
            let mut down_audio = tokio::spawn(async move {
                let mut errs: u32 = 0;
                loop {
                    match rx_audio.recv().await {
                        Ok(data) => {
                            if let Err(e) = conn_audio.send_datagram(data.as_slice()) {
                                errs += 1;
                                if errs == 1 || errs % 200 == 0 {
                                    eprintln!("[gw] 音频 send_datagram 失败（第 {errs} 次）: {e}");
                                }
                                if errs > 200 {
                                    break;
                                }
                                continue;
                            }
                            errs = 0;
                            AUDIO_OUT.fetch_add(1, Ordering::Relaxed);
                        }
                        Err(broadcast::error::RecvError::Lagged(n)) => {
                            /* 音频是连续流，落后就整段丢弃（下一次会带 RED 冗余补一帧） */
                            AUDIO_DROPPED.fetch_add(n, Ordering::Relaxed);
                        }
                        Err(_) => break,
                    }
                }
            });

            /* 上行：控制流 */
            let up = async {
                /* 只建了 WebTransport 会话却不建双向流的客户端不能永久占用 ACTIVE */
                let (mut send, mut recv) = match tokio::time::timeout(
                    std::time::Duration::from_secs(10),
                    connection.accept_bi(),
                ).await {
                    Ok(Ok(v)) => v,
                    _ => {
                        eprintln!("[gw] 会话未建立控制流（超时/失败）");
                        return None;
                    }
                };
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

                        /* 输入权归属：同一时刻只允许一个会话操作指针，避免
                           “多个鼠标打架”。但归属带 1.5s 租约：owner 空闲超时后
                           任何会话都能接管，这样没断干净的旧会话不会永久锁死鼠标。
                           用 compare_exchange 抢占，避免两个会话同时判定超时后
                           都以为自己拿权、互相覆盖。 */
                        if let Some(t) = ctrl_type(&msg) {
                            if is_input(t) {
                                let now = now_ms();
                                let owner = INPUT_OWNER.load(Ordering::Relaxed);
                                let busy = owner != 0
                                    && owner != sid
                                    && now.saturating_sub(INPUT_LAST.load(Ordering::Relaxed))
                                        < INPUT_LEASE_MS;
                                if busy {
                                    continue;
                                }
                                if owner != sid {
                                    /* 抢占失败说明另一个会话刚刚接管，这条输入让给它 */
                                    if INPUT_OWNER
                                        .compare_exchange(owner, sid, Ordering::Relaxed, Ordering::Relaxed)
                                        .is_err()
                                    {
                                        continue;
                                    }
                                }
                                INPUT_LAST.store(now, Ordering::Relaxed);
                            }
                        }
                        /* 应用层 RTT 探测：网关直接回 pong，不打扰 wwcap。
                           ping/pong 等长，直接在原 JSON 上把 "ping" 的 i 改成 o，
                           省掉一次 JSON 解析。 */
                        if let Some(t) = ctrl_type(&msg) {
                            if t == b"ping" {
                                let mut out = msg.clone();
                                if let Some(p) = out.windows(10).position(|w| w == br#""t":"ping""#) {
                                    out[p + 6] = b'o';
                                    let mut framed = Vec::with_capacity(4 + out.len());
                                    framed.extend_from_slice(&(out.len() as u32).to_le_bytes());
                                    framed.extend_from_slice(&out);
                                    let _ = send.write_all(&framed).await;
                                }
                                continue;
                            }
                        }
                        /* 音频冗余反馈：客户端每 500ms 报一次音频丢包率，
                           跨过阈值才动档位（局域网基本一直是 0%，弱网才开） */
                        if let Some(t) = ctrl_type(&msg) {
                            if t == b"astats" {
                                let loss = json_num(&msg, "aloss").unwrap_or(0.0);
                                let now = now_ms();
                                let cur = AUDIO_RED.load(Ordering::Relaxed);
                                let want = if loss > 0.02 {
                                    1
                                } else if loss < 0.002 {
                                    0
                                } else {
                                    cur
                                };
                                if want != cur
                                    && now.saturating_sub(AUDIO_RED_AT.load(Ordering::Relaxed))
                                        > AUDIO_RED_HOLD_MS
                                {
                                    AUDIO_RED.store(want, Ordering::Relaxed);
                                    AUDIO_RED_AT.store(now, Ordering::Relaxed);
                                    let m = format!("{{\"t\":\"ared\",\"n\":{want}}}");
                                    let _ = ctrl_sock.try_send_to(m.as_bytes(), &ctrl_addr);
                                    eprintln!("[gw] 音频丢包 {:.2}% → RED {want}", loss * 100.0);
                                }
                                continue;   // astats 只给网关看，不转发给 wwcap
                            }
                        }
                        /* 客户端主动重启流服务（黑屏/卡死自救）。
                           只执行固定脚本、参数只有 deep 0/1，不接受任何路径或命令。 */
                        if let Some(t) = ctrl_type(&msg) {
                            if t == b"restart" {
                                let now = now_ms();
                                if now.saturating_sub(RESTART_AT.load(Ordering::Relaxed))
                                    < RESTART_HOLD_MS
                                {
                                    let _ = ctrl_reply(
                                        &mut send,
                                        "{\"t\":\"restart\",\"stage\":\"busy\"}",
                                    )
                                    .await;
                                    continue;
                                }
                                RESTART_AT.store(now, Ordering::Relaxed);
                                RESTARTS.fetch_add(1, Ordering::Relaxed);

                                let deep = json_num(&msg, "deep").unwrap_or(0.0) >= 1.0;
                                let script = std::env::var("WW_RESTART_SCRIPT")
                                    .unwrap_or_else(|_| "scripts/restart-capture.sh".to_string());
                                let mut path = std::path::PathBuf::from(&script);
                                if path.is_relative() {
                                    path = std::env::current_dir().unwrap_or_default().join(path);
                                }
                                eprintln!(
                                    "[gw] 客户端请求重启流服务（deep={deep}）: {}",
                                    path.display()
                                );
                                let _ = ctrl_reply(
                                    &mut send,
                                    if deep {
                                        "{\"t\":\"restart\",\"stage\":\"starting\",\"deep\":1}"
                                    } else {
                                        "{\"t\":\"restart\",\"stage\":\"starting\"}"
                                    },
                                )
                                .await;

                                let cs = ctrl_sock.clone();
                                let ca = ctrl_addr.clone();
                                tokio::spawn(async move {
                                    let ok = if path.is_file() {
                                        let mut cmd = tokio::process::Command::new("bash");
                                        cmd.arg(&path)
                                            .stdout(std::process::Stdio::null())
                                            .stderr(std::process::Stdio::null());
                                        if deep {
                                            cmd.arg("--deep");
                                        }
                                        matches!(cmd.status().await, Ok(s) if s.success())
                                    } else {
                                        eprintln!("[gw] 重启脚本不存在: {}", path.display());
                                        false
                                    };
                                    /* 新进程默认不采集音频、也还没有新的 IDR：
                                       隔几个时间点补发，覆盖「秒级重启」和「deep 十几秒重建」 */
                                    for ms in [800u64, 3000, 8000] {
                                        tokio::time::sleep(std::time::Duration::from_millis(ms))
                                            .await;
                                        let _ = cs.send_to(b"{\"t\":\"kf\"}", &ca).await;
                                        let _ = cs
                                            .send_to(b"{\"t\":\"astream\",\"rx\":1}", &ca)
                                            .await;
                                        /* 新进程默认无会话：补发 active，否则 30 秒后自己睡回去 */
                                        let _ = cs
                                            .send_to(b"{\"t\":\"active\",\"v\":1}", &ca)
                                            .await;
                                    }
                                    eprintln!(
                                        "[gw] 重启流服务{}",
                                        if ok { "完成" } else { "脚本返回失败" }
                                    );
                                });
                                continue;
                            }
                        }
                        /* 用非阻塞发送：控制面不该被 Unix socket 背压拖住 */
                        let _ = ctrl_sock.try_send_to(&msg, &ctrl_addr);
                    }
                }
                let _ = send.finish().await;
                Some(())
            };

            tokio::select! {
                _ = &mut down => {},
                _ = &mut down_audio => {},
                _ = up => {},
            }
            /* 另一侧必须显式 abort：select 只是 detach，不 abort 会泄漏一个永久任务 */
            down.abort();
            down_audio.abort();
            /* 控制者退出：释放所有按下的键/修饰键，避免 SUPER/CTRL 卡死在远端 */
            if INPUT_OWNER.compare_exchange(sid, 0, Ordering::Relaxed, Ordering::Relaxed).is_ok() {
                INPUT_LAST.store(0, Ordering::Relaxed);
                let _ = ctrl_sock.send_to(b"{\"t\":\"keysreset\"}", &ctrl_addr).await;
            }
            /* 最后一个客户端离开：让桌面回到默认 1080p / 默认帧率，
               别把上一个会话（或测试脚本）选的 30fps 留给下一个用户 */
            if ACTIVE.fetch_sub(1, Ordering::Relaxed) <= 1 {
                let _ = ctrl_sock.send_to(b"{\"t\":\"mode\",\"reset\":1}", &ctrl_addr).await;
                let _ = ctrl_sock.send_to(b"{\"t\":\"fps\",\"v\":0}", &ctrl_addr).await;
                /* 没人看了就停采集，并把目标机扬声器恢复原状态 */
                let _ = ctrl_sock.send_to(b"{\"t\":\"astream\",\"rx\":0}", &ctrl_addr).await;
                let _ = ctrl_sock.send_to(b"{\"t\":\"mute\",\"on\":0}", &ctrl_addr).await;
                /* 告诉 wwcap 会话已清空：空闲 30 秒后它会休眠，不再空转编码器 */
                let _ = ctrl_sock.send_to(b"{\"t\":\"active\",\"v\":0}", &ctrl_addr).await;
            }
            eprintln!("[gw] 会话结束 {peer}（#{sid}）");
        });
    }
}
