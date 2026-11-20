// 原生客户端的 UDP 传输（8444/udp）
//
// 为什么单开一条传输：原生客户端不需要 QUIC/TLS 的可靠流与拥塞控制，
// 视频本来就是不可靠数据报；直接 UDP 能把网关的转发路径压到"收包 → 复制 → 发包"，
// 少一层 QUIC 发送队列，也避免 WebTransport 对单个 datagram 的拥塞窗口约束。
//
// 信封（8 字节，小端）：
//   0..2  magic "WN"
//   2     type
//   3     flags（保留）
//   4..8  seq（下行用于丢包统计，上行原样回显给 pong）
//   8..   payload
//
// 视频 payload 就是 proto/wire.h 的 24B 分片头 + 数据，浏览器与原生客户端共用同一套
// 分片/重组逻辑，服务端不需要为两端编码两次。

use std::net::SocketAddr;
use std::path::PathBuf;
use std::sync::atomic::Ordering;
use std::sync::Arc;
use std::time::{Duration, Instant};

use anyhow::Result;
use tokio::net::{UdpSocket, UnixDatagram};
use tokio::sync::broadcast;

use crate::{now_ms, ACTIVE, INPUT_LAST, INPUT_LEASE_MS, INPUT_OWNER};

pub const PORT: u16 = 8444;

const MAGIC: [u8; 2] = [b'W', b'N'];
const HDR: usize = 8;

pub const T_VIDEO: u8 = 1;   /* S→C 视频分片 */
pub const T_AUDIO: u8 = 2;   /* S→C 音频包 */
#[allow(dead_code)] /* 预留：客户端侧解析下行 JSON 时才会用到 */
pub const T_CTRL: u8 = 3;    /* S→C JSON */
pub const T_CTRL_UP: u8 = 4; /* C→S JSON */
pub const T_HELLO: u8 = 5;   /* C→S，1 秒一次的保活 */
pub const T_HELLO_ACK: u8 = 6;
pub const T_PING: u8 = 7;
pub const T_PONG: u8 = 8;
pub const T_STATS: u8 = 9;   /* C→S，客户端自报指标（仅日志） */

/* 输入归属用独立会话号，避免和 WebTransport 的 sid 冲突 */
const NATIVE_SID: u64 = 0xFFFF_FFFF;
const PEER_TIMEOUT: Duration = Duration::from_secs(5);

struct Peer {
    addr: SocketAddr,
    last_seen: Instant,
    /* 是否已经计入 ACTIVE：重复 HELLO 只是保活，不能反复 +1 */
    counted: bool,
}

fn put_hdr(out: &mut Vec<u8>, t: u8, seq: u32, payload_len: usize) {
    out.clear();
    out.reserve(HDR + payload_len);
    out.extend_from_slice(&MAGIC);
    out.push(t);
    out.push(0);
    out.extend_from_slice(&seq.to_le_bytes());
}

fn read_seq(pkt: &[u8]) -> u32 {
    u32::from_le_bytes([pkt[4], pkt[5], pkt[6], pkt[7]])
}

/* 原生客户端输入也要遵守和浏览器相同的 1.5 秒租约：
   否则浏览器和原生端同时动鼠标会互相拽光标。 */
fn claim_input() -> bool {
    let now = now_ms();
    let owner = INPUT_OWNER.load(Ordering::Relaxed);
    let busy = owner != 0
        && owner != NATIVE_SID
        && now.saturating_sub(INPUT_LAST.load(Ordering::Relaxed)) < INPUT_LEASE_MS;
    if busy {
        return false;
    }
    if owner != NATIVE_SID
        && INPUT_OWNER
            .compare_exchange(owner, NATIVE_SID, Ordering::Relaxed, Ordering::Relaxed)
            .is_err()
    {
        return false;
    }
    INPUT_LAST.store(now, Ordering::Relaxed);
    true
}

async fn release_and_idle(ctrl_sock: &UnixDatagram, ctrl_addr: &PathBuf) {
    let _ = ctrl_sock.send_to(b"{\"t\":\"keysreset\"}", ctrl_addr).await;
    if INPUT_OWNER
        .compare_exchange(NATIVE_SID, 0, Ordering::Relaxed, Ordering::Relaxed)
        .is_ok()
    {
        INPUT_LAST.store(0, Ordering::Relaxed);
    }
    if ACTIVE.fetch_sub(1, Ordering::Relaxed) <= 1 {
        let _ = ctrl_sock.send_to(b"{\"t\":\"mode\",\"reset\":1}", ctrl_addr).await;
        let _ = ctrl_sock.send_to(b"{\"t\":\"fps\",\"v\":0}", ctrl_addr).await;
        let _ = ctrl_sock.send_to(b"{\"t\":\"astream\",\"rx\":0}", ctrl_addr).await;
        let _ = ctrl_sock.send_to(b"{\"t\":\"active\",\"v\":0}", ctrl_addr).await;
    }
}

pub async fn serve(
    tx: broadcast::Sender<Arc<Vec<u8>>>,
    tx_audio: broadcast::Sender<Arc<Vec<u8>>>,
    ctrl_sock: Arc<UnixDatagram>,
    ctrl_addr: PathBuf,
    caps_path: PathBuf,
) -> Result<()> {
    let sock = Arc::new(UdpSocket::bind(("0.0.0.0", PORT)).await?);
    eprintln!("[gw] 原生客户端 UDP 监听 :{PORT}/udp");

    let mut rx_video = tx.subscribe();
    let mut rx_audio = tx_audio.subscribe();
    let mut buf = vec![0u8; 2048];
    let mut out = Vec::with_capacity(1600);
    let mut peer: Option<Peer> = None;
    let mut vid_seq: u32 = 0;
    let mut aud_seq: u32 = 0;
    let mut house = tokio::time::interval(Duration::from_millis(500));
    /* 视频/音频到达时 peer 可能为空：直接丢，不排队。
       排队只会把"无人观看时的旧画面"在连上后一次性灌给客户端，延迟反而更大。 */
    loop {
        tokio::select! {
            _ = house.tick() => {
                if peer.as_ref().map(|p| p.last_seen.elapsed() > PEER_TIMEOUT).unwrap_or(false) {
                    let p = peer.take().unwrap();
                    if p.counted {
                        release_and_idle(&ctrl_sock, &ctrl_addr).await;
                    }
                    eprintln!("[gw] 原生客户端 {} 超时断开", p.addr);
                }
            }
            r = sock.recv_from(&mut buf) => {
                let (n, addr) = match r {
                    Ok(v) => v,
                    Err(_) => continue,
                };
                if n < HDR || buf[0..2] != MAGIC {
                    continue;
                }
                let t = buf[2];
                let seq = read_seq(&buf[..n]);
                let payload = &buf[HDR..n];
                match t {
                    T_HELLO => {
                        let is_new = peer.as_ref().map(|p| p.addr) != Some(addr);
                        if is_new {
                            if let Some(old) = peer.take() {
                                if old.counted {
                                    release_and_idle(&ctrl_sock, &ctrl_addr).await;
                                }
                            }
                            peer = Some(Peer { addr, last_seen: Instant::now(), counted: true });
                            ACTIVE.fetch_add(1, Ordering::Relaxed);
                            eprintln!("[gw] 原生客户端接入 {addr}");

                            let caps = tokio::fs::read_to_string(&caps_path)
                                .await
                                .unwrap_or_else(|_| "{}".into());
                            let json = format!(
                                "{{\"t\":\"hello_ack\",\"proto\":1,\"port\":{PORT},\"caps\":{}}}",
                                caps.trim()
                            );
                            put_hdr(&mut out, T_HELLO_ACK, vid_seq, json.len());
                            out.extend_from_slice(json.as_bytes());
                            let _ = sock.send_to(&out, addr).await;

                            /* 新客户端的第一帧必须是 IDR；同时唤醒采集与音频 */
                            let _ = ctrl_sock.send_to(b"{\"t\":\"kf\"}", &ctrl_addr).await;
                            let _ = ctrl_sock
                                .send_to(b"{\"t\":\"astream\",\"rx\":1}", &ctrl_addr)
                                .await;
                            let _ = ctrl_sock
                                .send_to(b"{\"t\":\"active\",\"v\":1}", &ctrl_addr)
                                .await;
                        } else if let Some(p) = peer.as_mut() {
                            p.last_seen = Instant::now();
                        }
                    }
                    T_PING => {
                        if let Some(p) = peer.as_mut() {
                            if p.addr == addr {
                                p.last_seen = Instant::now();
                                put_hdr(&mut out, T_PONG, seq, 0);
                                let _ = sock.send_to(&out, addr).await;
                            }
                        }
                    }
                    T_CTRL_UP => {
                        let Some(p) = peer.as_mut() else {
                            continue;
                        };
                        if p.addr != addr {
                            continue;
                        }
                        p.last_seen = Instant::now();
                        if payload.is_empty() {
                            continue;
                        }
                        let msg = payload.to_vec();
                        /* 输入消息要先抢租约，避免与浏览器会话互相拽光标 */
                        let is_input = crate::ctrl_type(&msg)
                            .map(crate::is_input)
                            .unwrap_or(false);
                        if is_input && !claim_input() {
                            continue;
                        }
                        let _ = ctrl_sock.try_send_to(&msg, &ctrl_addr);
                    }
                    T_STATS => {
                        if let Some(p) = peer.as_mut() {
                            if p.addr == addr {
                                p.last_seen = Instant::now();
                            }
                        }
                        if let Ok(s) = std::str::from_utf8(payload) {
                            if s.len() < 512 && !s.is_empty() {
                                eprintln!("[gw] 原生客户端指标 {s}");
                            }
                        }
                    }
                    _ => {}
                }
            }
            r = rx_video.recv() => {
                let Ok(data) = r else { continue };
                let Some(p) = peer.as_ref() else { continue };
                vid_seq = vid_seq.wrapping_add(1);
                put_hdr(&mut out, T_VIDEO, vid_seq, data.len());
                out.extend_from_slice(&data);
                if sock.send_to(&out, p.addr).await.is_err() {
                    /* 单个分片失败不重建会话：ICMP 端口不可达可能只是瞬时抖动 */
                }
            }
            r = rx_audio.recv() => {
                let Ok(data) = r else { continue };
                let Some(p) = peer.as_ref() else { continue };
                aud_seq = aud_seq.wrapping_add(1);
                put_hdr(&mut out, T_AUDIO, aud_seq, data.len());
                out.extend_from_slice(&data);
                let _ = sock.send_to(&out, p.addr).await;
            }
        }
    }
}
