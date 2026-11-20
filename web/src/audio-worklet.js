/* 音频播放工作线程：环形缓冲 + 缺口淡出
 *
 * 为什么不用 <audio>/MediaStreamTrackGenerator：音频要做主时钟，
 * 必须能拿到「已经播到哪里」和「还剩多少」两个量，只有 AudioWorklet 能同时给。
 *
 * 缺口处理：WebCodecs 不暴露 Opus 的 decode_fec，单包丢失由服务端 RED
 * 冗余完整补回；连续丢包时这里做淡出，避免硬静音造成爆音。
 */
class WwAudioProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const o = options.processorOptions || {};
    this.channels = o.channels || 2;
    this.targetMs = o.targetMs || 40;
    /* 容量 = 目标水位 + 160ms 余量；再多的就只有丢弃（低延迟优先） */
    this.cap = Math.max(1024, Math.ceil((sampleRate * (this.targetMs + 160)) / 1000));
    this.ring = [new Float32Array(this.cap), new Float32Array(this.cap)];
    this.w = 0;
    this.r = 0;
    this.fill = 0;
    this.last = new Float32Array(this.channels);
    this.stalled = true;   // 起播前先攒够目标水位
    this.plc = 0;          // 待遮挡的采样数
    this.underrun = 0;     // 本上报周期内的空读次数
    this.acc = 0;          // 距上次上报已处理的采样数
    this.port.onmessage = (e) => this.#onMessage(e.data);
    this.port.postMessage({ type: 'ready', sampleRate, capMs: (this.cap / sampleRate) * 1000 });
  }

  #onMessage(m) {
    if (!m) return;
    switch (m.type) {
      case 'push': this.#push(m.ch0, m.ch1, m.frames); break;
      case 'gap': this.plc += m.frames | 0; break;
      case 'target': this.targetMs = m.ms; break;
      case 'reset':
        this.w = this.r = this.fill = 0;
        this.plc = 0;
        this.stalled = true;
        this.last.fill(0);
        break;
      default: break;
    }
  }

  #push(a, b, n) {
    if (!n) return;
    const cap = this.cap;
    if (this.fill + n > cap) {
      /* 溢出说明客户端解码/调度跟不上：丢最老的，保持低延迟而不是越攒越多 */
      const drop = this.fill + n - cap;
      this.r = (this.r + drop) % cap;
      this.fill -= drop;
      this.port.postMessage({ type: 'overflow', drop });
    }
    for (let i = 0; i < n; i++) {
      const p = (this.w + i) % cap;
      this.ring[0][p] = a[i];
      this.ring[1][p] = b[i];
    }
    this.w = (this.w + n) % cap;
    this.fill += n;
  }

  process(inputs, outputs) {
    const out = outputs[0];
    if (!out || !out.length) return true;
    const n = out[0].length;
    const cap = this.cap;
    const nch = out.length;
    const need = Math.round((sampleRate * this.targetMs) / 1000);

    /* 起播/欠载恢复：先攒够目标水位再出声。
       这一步是「自适应缓冲」真正生效的地方——targetMs 变大，攒的时间就变长。 */
    if (this.stalled && this.fill >= need) this.stalled = false;

    if (this.stalled) {
      for (let c = 0; c < nch; c++) {
        this.last[c] = 0;
        out[c].fill(0);
      }
      this.acc += n;
      if (this.acc >= sampleRate / 10) {
        this.acc = 0;
        this.port.postMessage({ type: 'level', ms: (this.fill / sampleRate) * 1000, underrun: 0, stalled: true });
      }
      return true;
    }

    /* 读指针是所有声道共用的，因此按「帧」循环，一帧内把所有声道一起取出 */
    for (let i = 0; i < n; i++) {
      if (this.fill > 0) {
        for (let c = 0; c < nch; c++) {
          const v = (this.ring[c] || this.ring[0])[this.r];
          out[c][i] = v;
          this.last[c] = v;
        }
        this.r = (this.r + 1) % cap;
        this.fill--;
        if (this.fill === 0) {
          /* 读空：进入欠载，下一次攒够目标水位才继续，避免逐帧咔哒 */
          this.stalled = true;
          if (this.plc === 0) this.underrun++;
        }
      } else if (this.plc > 0) {
        /* 丢包区间：指数淡出，避免从最后一个采样直接掉到 0 的爆音 */
        for (let c = 0; c < nch; c++) {
          this.last[c] *= 0.94;
          out[c][i] = this.last[c];
        }
        this.plc--;
      } else {
        for (let c = 0; c < nch; c++) {
          this.last[c] = 0;
          out[c][i] = 0;
        }
      }
    }

    this.acc += n;
    if (this.acc >= sampleRate / 10) {   // 每 100ms 上报一次水位
      this.acc = 0;
      this.port.postMessage({
        type: 'level',
        ms: (this.fill / sampleRate) * 1000,
        underrun: this.underrun,
        currentFrame,
      });
      this.underrun = 0;
    }
    return true;
  }
}

registerProcessor('ww-audio', WwAudioProcessor);
