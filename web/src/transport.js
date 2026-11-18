// WebTransport 封装：datagram 分片重组 + 可靠控制流
import { VID_HDR_SIZE, WIRE_VERSION, parseFrag, encodeCtrl } from './proto.js';

export class Transport {
  constructor(url, hooks = {}, options = {}) {
    this.url = url;
    this.options = options;       // { serverCertificateHashes: [...] }
    this.hooks = hooks;          // { onChunk(chunk), onClose(), onError(e) }
    this.pending = new Map();     // frameId -> {frags, count, key, pts}
    this.pendingOrder = [];       // 插入顺序，用于 LRU 裁剪
    this.stats = { datagrams: 0, bytes: 0, lost: 0 };
    this.ctrlQueue = [];
    this.closed = false;
    this.connected = false;
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
      f = { frags: new Array(h.fragCount), have: 0, count: h.fragCount, key: !!(h.flags & 0x01), pts: h.ptsUs };
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
      if (this.pending.delete(id)) this.stats.lost++;
    }
  }

  #emit(f) {
    let len = 0;
    for (const p of f.frags) len += p.length;
    const data = new Uint8Array(len);
    let off = 0;
    for (const p of f.frags) { data.set(p, off); off += p.length; }
    this.hooks.onChunk?.({ data, key: f.key, ptsUs: f.pts, arrivedAt: performance.now() });
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
