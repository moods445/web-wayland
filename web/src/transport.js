// WebTransport 封装：datagram 分片重组 + 可靠控制流
import { AUDIO_VERSION, VID_HDR_SIZE, WIRE_VERSION, parseFrag, encodeCtrl } from './proto.js';

export class Transport {
  constructor(url, hooks = {}, options = {}) {
    this.url = url;
    this.options = options;       // { serverCertificateHashes: [...] }
    this.hooks = hooks;          // { onChunk(chunk), onClose(), onError(e) }
    this.pending = new Map();     // frameId -> {frags, count, key, pts}
    this.pendingOrder = [];       // 插入顺序，用于 LRU 裁剪
    this.stats = {
      datagrams: 0, bytes: 0, lost: 0,
      frames: 0,       // 成功重组并交给解码器的帧
      incomplete: 0,   // 分片缺失/裁剪导致整帧丢弃
      late: 0,         // 乱序迟到、参考链已过而丢弃的帧
      audio: { packets: 0, bytes: 0 },   // 音频包只统计到传输层，丢包在 AudioPlayer 里算
    };
    this.ctrlQueue = [];
    this.closed = false;
    this.connected = false;
    this.lastFrameId = null;      // 已交给解码器的最大 frameId（datagram 会乱序）
  }

  /* uint32 帧号回绕安全的“更新”判断 */
  static #newer(a, b) {
    return a !== b && ((a - b) >>> 0) < 0x80000000;
  }

  async connect() {
    this.wt = new WebTransport(this.url, this.options);
    await this.wt.ready;
    this.connected = true;

    this.readLoop = this.#readDatagrams();
    this.#openCtrl();
    this.wt.closed.then(() => this.#shutdown('closed')).catch(() => this.#shutdown('error'));
  }

  #shutdown(reason) {
    if (this.closed) return;
    this.closed = true;
    this.connected = false;
    this.lastFrameId = null;
    this.pending.clear();
    this.pendingOrder.length = 0;
    try { this.ctrlWriter?.close?.(); } catch { /* noop */ }
    this.hooks.onClose?.(reason);
  }

  async #readDatagrams() {
    const reader = this.wt.datagrams.readable.getReader();
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        this.#onDatagram(value);
      }
    } catch (e) {
      this.hooks.onError?.(e);
    }
  }

  #onDatagram(buf) {
    if (buf.byteLength < 1) return;

    /* 音视频共用一个 datagram 通道，靠首字节分流：
       2 = 音频包（不进视频重组，避免 16B 小包污染 pending Map）。
       注意 buf 是 Uint8Array，取值只能下标访问（没有 getUint8，那是 DataView 的方法）。 */
    if (buf[0] === AUDIO_VERSION) {
      this.stats.audio.packets++;
      this.stats.audio.bytes += buf.byteLength;
      this.hooks.onAudio?.(buf);
      return;
    }
    if (buf.byteLength < VID_HDR_SIZE) return;

    const h = parseFrag(buf);

    /* 严格校验：版本、分片数量、索引、载荷长度都必须自洽，否则直接丢包 */
    if (h.version !== WIRE_VERSION) return;
    if (h.fragCount === 0 || h.fragIndex >= h.fragCount) { this.stats.lost++; return; }
    const avail = buf.byteLength - VID_HDR_SIZE;
    if (h.payloadLen > avail) { this.stats.lost++; return; }

    this.stats.datagrams++;
    this.stats.bytes += buf.byteLength;

    const payload = buf.subarray(VID_HDR_SIZE, VID_HDR_SIZE + h.payloadLen);

    let f = this.pending.get(h.frameId);
    if (!f) {
      f = { id: h.frameId, frags: new Array(h.fragCount), have: 0, count: h.fragCount, key: !!(h.flags & 0x01), pts: h.ptsUs };
      this.pending.set(h.frameId, f);
      this.pendingOrder.push(h.frameId);
      this.#trimPending();
    }
    if (f.count !== h.fragCount) return;   // 同一 frameId 的约束不一致，丢弃
    if (!f.frags[h.fragIndex]) {
      f.frags[h.fragIndex] = payload;
      f.have++;
    }

    if (f.have === f.count) {
      this.pending.delete(h.frameId);
      this.#emit(f);
    }
  }

  /* LRU 裁剪：按插入顺序淘汰，避免持续丢片时 Map 无界增长 */
  #trimPending() {
    while (this.pendingOrder.length > 120) {
      const id = this.pendingOrder.shift();
      if (this.pending.delete(id)) { this.stats.lost++; this.stats.incomplete++; }
    }
  }

  #emit(f) {
    /* QUIC datagram 不保证顺序：后一帧可能先凑齐。迟到帧如果还送给解码器，
       会让 H.264 参考链倒退（花屏），所以只按 frameId 单调递增交付。
       若倒退太多（>300 帧），视为 wwcap 重启、帧号归零，重新接受。 */
    if (this.lastFrameId !== null) {
      if (!Transport.#newer(f.id, this.lastFrameId)) {
        if (((this.lastFrameId - f.id) >>> 0) <= 300) {
          this.stats.lost++;
          this.stats.late++;
          return;
        }
      }
    }
    this.lastFrameId = f.id;
    this.stats.frames++;
    let len = 0;
    for (const p of f.frags) len += p.length;
    const data = new Uint8Array(len);
    let off = 0;
    for (const p of f.frags) { data.set(p, off); off += p.length; }
    this.hooks.onChunk?.({ data, key: f.key, ptsUs: f.pts, frameId: f.id, arrivedAt: performance.now() });
  }

  /* ── 控制流 ── */
  async #openCtrl() {
    try {
      const stream = await this.wt.createBidirectionalStream();
      this.ctrlWriter = stream.writable.getWriter();
      this.#readCtrl(stream.readable);
      // 补齐就绪前排队发送的消息（hello/stats 不能丢）
      for (const m of this.ctrlQueue) this.send(m);
      this.ctrlQueue.length = 0;
    } catch (e) {
      console.error('ww: 控制流建立失败', e?.name, e?.message ?? e);
      this.hooks.onError?.(e);
    }
  }

  async #readCtrl(readable) {
    const reader = readable.getReader();
    let acc = new Uint8Array(0);
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        const merged = new Uint8Array(acc.length + value.length);
        merged.set(acc); merged.set(value, acc.length);
        acc = merged;
        while (acc.length >= 4) {
          const len = new DataView(acc.buffer, acc.byteOffset, 4).getUint32(0, true);
          if (acc.length < 4 + len) break;
          const msg = JSON.parse(new TextDecoder().decode(acc.subarray(4, 4 + len)));
          acc = acc.subarray(4 + len);
          this.hooks.onCtrl?.(msg);
        }
      }
    } catch (e) {
      this.hooks.onError?.(e);
    }
  }

  send(obj) {
    if (this.closed) return;    // 断线后 input 仍可能触发，直接丢弃
    if (!this.ctrlWriter) {
      if (this.ctrlQueue.length < 64) this.ctrlQueue.push(obj);
      return;
    }
    this.ctrlWriter.write(encodeCtrl(obj)).catch(() => {});
  }

  close() {
    try { this.wt?.close(); } catch { /* noop */ }
    this.#shutdown('local');
  }
}
