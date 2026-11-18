// 从 Annex-B 码流的 SPS 推导准确的 codec 字符串（profile/constraints/level）
export function codecFromAnnexB(data) {
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
      // constraint 字节统一写 00（厂商私有位会被 Chrome 判为不支持），level 上限 5.1
      return `avc1.${h(profile)}00${h(Math.min(level, 0x33))}`;
    }
  }
  return 'avc1.640033';
}

// WebCodecs 解码：Annex-B 直喂，H.264 High
export class Decoder {
  constructor({ onFrame, onError, onNeedKeyframe }) {
    this.onFrame = onFrame;
    this.onError = onError;
    this.onNeedKeyframe = onNeedKeyframe;
    this.decoded = 0;
    this.dropped = 0;
    this.skipped = 0;   // 因解码队列积压而主动丢弃的帧
    this.lastTs = 0;
    this.frameInterval = 16666;
    this.configured = false;
    this.gotKey = false;
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
    this.decoder.configure({
      codec,
      avc: { format: 'annexb' },
      codedWidth: width,
      codedHeight: height,
      optimizeForLatency: true,
      // 不强制硬件解码：无 GPU 环境下 prefer-hardware 会直接失败
    });
    this.configured = true;
    this.gotKey = false;   // 配置后必须等一个 IDR，否则 decode() 会抛 DataError
    this.lastTs = 0;
  }

  #onOutput(frame) {
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
    // 时间戳必须严格递增，否则 Chrome 会抛 DataError 并关闭解码器
    let ts = ptsUs;
    if (ts <= this.prevTs) ts = this.prevTs + 1;
    this.prevTs = ts;

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
}
