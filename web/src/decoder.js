// 从 Annex-B 码流的 SPS 推导准确的 codec 字符串（profile/constraints/level）
export function codecFromAnnexB(data, strict = false) {
  for (let i = 0; i + 5 < data.length; i++) {
    if (data[i] !== 0 || data[i + 1] !== 0) continue;
    let off = -1;
    if (data[i + 2] === 1) off = i + 3;
    else if (data[i + 2] === 0 && data[i + 3] === 1) off = i + 4;
    if (off < 0) continue;
    const type = data[off] & 0x1f;
    if (type === 7 && off + 3 < data.length) {
      const h = (n) => n.toString(16).padStart(2, '0');
      const profile = data[off + 1];
      const level = data[off + 3];
      // constraint 字节统一写 00（厂商私有位会被 Chrome 判为不支持）。
      // level 必须如实输出：4K60 的 High@L5.2 如果被压成 5.1，
      // 部分 Chrome/硬解会直接拒绝配置或退回软解。
      return `avc1.${h(profile)}00${h(level)}`;
    }
  }
  /* strict 用于「这次分片里是否真的带 SPS」的判断：没带返回 null，
     避免把不含 SPS 的关键帧误判成 profile 变化而反复重建解码器 */
  return strict ? null : 'avc1.640033';
}

// WebCodecs 解码：Annex-B 直喂，H.264 High
export class Decoder {
  constructor({ onFrame, onError, onNeedKeyframe, hw = 'auto' }) {
    this.onFrame = onFrame;
    this.onError = onError;
    this.onNeedKeyframe = onNeedKeyframe;
    /* auto | hardware | software：映射到 WebCodecs 的 hardwareAcceleration 偏好。
       auto 不传该字段，交给浏览器自行选择。 */
    this.hw = hw;
    this.decoded = 0;
    this.dropped = 0;
    this.skipped = 0;   // 因解码队列积压而主动丢弃的帧
    this.lastTs = 0;
    this.frameInterval = 16666;
    this.configured = false;
    this.gotKey = false;
    /* 统计面板用：入队时刻 + 当时队列深度，输出时折算解码/排队耗时 */
    this.enqueued = new Map();
    this.timing = { decodeSum: 0, decodeN: 0, queueSum: 0, queueN: 0 };
  }

  configure({ width, height, fps = 60, codec = 'avc1.640028' }) {
    this.frameInterval = Math.max(1000, Math.floor(1e6 / fps));
    if (this.decoder) this.decoder.close();
    this.prevTs = 0;
    this.decoder = new VideoDecoder({
      output: (frame) => this.#onOutput(frame),
      error: (e) => {
        // 解码器出错后会被关闭，必须重建才能继续
        this.configured = false;
        this.onError?.(e);
      },
    });
    const base = {
      codec,
      avc: { format: 'annexb' },
      codedWidth: width,
      codedHeight: height,
      optimizeForLatency: true,
    };
    if (this.hw === 'hardware') base.hardwareAcceleration = 'prefer-hardware';
    else if (this.hw === 'software') base.hardwareAcceleration = 'prefer-software';
    try {
      this.decoder.configure(base);
    } catch (e) {
      /* 厂商私有 SPS / level 不被接受时退化到最保守的 High@4.0 再试一次，
         否则异常会冒泡到 datagram 读循环，整个串流静默停住。
         硬件偏好也可能直接被拒，回退时不再带 hardwareAcceleration。 */
      /* 4:4:4 流必须回退到 High 4:4:4 的保守 level，不能退回 4:2:0 的 High */
      const fallback = parseInt(codec.slice(5, 7), 16) === 0xf4 ? 'avc1.f40028' : 'avc1.640028';
      console.warn(`ww: configure 失败，回退 ${fallback}`, e?.message ?? e);
      this.decoder.configure({
        codec: fallback,
        avc: { format: 'annexb' },
        codedWidth: width,
        codedHeight: height,
        optimizeForLatency: true,
      });
    }
    this.configured = true;
    this.gotKey = false;   // 配置后必须等一个 IDR，否则 decode() 会抛 DataError
    this.lastTs = 0;
    this.enqueued.clear();
    this.timing = { decodeSum: 0, decodeN: 0, queueSum: 0, queueN: 0 };
  }

  #onOutput(frame) {
    const rec = this.enqueued.get(frame.timestamp);
    if (rec) {
      this.enqueued.delete(frame.timestamp);
      const total = performance.now() - rec.at;
      const queue = rec.ahead * (this.frameInterval / 1000);
      this.timing.queueSum += queue;
      this.timing.queueN++;
      this.timing.decodeSum += Math.max(0, total - queue);
      this.timing.decodeN++;
    }
    // 主动丢弃过期帧，避免解码队列累积成"越用越卡"
    if (this.lastTs && frame.timestamp < this.lastTs - this.frameInterval * 2) {
      this.dropped++;
      frame.close();
      return;
    }
    this.lastTs = frame.timestamp;
    this.decoded++;
    this.onFrame(frame);
  }

  push({ data, key, ptsUs }) {
    if (!this.configured) return;
    if (!this.gotKey) {
      if (!key) return;      // 等首个关键帧
      this.gotKey = true;
    }

    /* 解码跟不上时必须丢帧，否则队列累积 = 延迟持续增长（越用越卡） */
    if (this.decoder.decodeQueueSize > 3) {
      if (!key) {
        this.skipped++;
        this.onNeedKeyframe?.();
        return;
      }
    }
    /* 时间戳必须严格递增（Chrome 会抛 DataError）。
       QUIC datagram 可能乱序，迟到的旧帧不能“拨快时间戳”硬塞进去，
       否则会把旧帧当新帧解码，破坏参考链导致花屏 —— 直接丢弃。 */
    let ts = ptsUs;
    if (ts <= this.prevTs) {
      this.skipped++;
      return;
    }
    this.prevTs = ts;

    /* 记录入队时刻与当时队列深度，供统计面板折算解码/排队耗时 */
    this.enqueued.set(ts, { at: performance.now(), ahead: this.decoder.decodeQueueSize });
    if (this.enqueued.size > 120) {
      this.enqueued.delete(this.enqueued.keys().next().value);
    }

    try {
      this.decoder.decode(new EncodedVideoChunk({
        type: key ? 'key' : 'delta',
        timestamp: ts,
        data,
      }));
    } catch (e) {
      this.onError?.(e);
    }
  }

  get queue() {
    return this.decoder?.decodeQueueSize ?? 0;
  }

  get decodeMs() {
    return this.timing.decodeN ? this.timing.decodeSum / this.timing.decodeN : null;
  }

  get queueMs() {
    return this.timing.queueN ? this.timing.queueSum / this.timing.queueN : null;
  }
}
