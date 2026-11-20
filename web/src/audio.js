/* 音频下行播放：Opus(AudioDecoder) → 重采样（仅在需要时）→ AudioWorklet → 扬声器
 *
 * 低延迟策略：
 *  - 初始水位 40ms（2 × 20ms 帧），水位过低才上调（上限 120ms），长期过高自动回落
 *  - 序号即时间轴：seq 相差 1 用包内 RED 冗余完整补回；更大缺口淡出遮挡
 *  - 只按 seq 单调前进，迟到包直接丢（不做重排等待，避免把延迟换成完整性）
 */
import { AUDIO_FRAME_MS, AUDIO_HDR_SIZE, AUDIO_VERSION, parseAudio } from './proto.js';

const RATE = 48000;
const CH = 2;
const TARGET_MS = 40;

/* Opus 的 WebCodecs 描述：19 字节 OpusHead（RFC 7845 §5.1） */
function opusHead(channels, rate) {
  const b = new Uint8Array(19);
  b.set(new TextEncoder().encode('OpusHead'), 0);
  b[8] = 1;                      /* version */
  b[9] = channels;
  const dv = new DataView(b.buffer);
  dv.setUint16(10, 0, true);     /* pre-skip = 0：低延迟优先，不吃掉开头 6.5ms */
  dv.setUint32(12, rate, true);
  dv.setInt16(16, 0, true);      /* output gain */
  b[18] = 0;                     /* mapping family 0：单流立体声 */
  return b;
}

export class AudioPlayer {
  constructor() {
    this.ctx = null;
    this.node = null;
    this.gain = null;
    this.decoder = null;
    this.ready = false;
    this.error = null;
    this.volume = 1;
    this.muted = false;
    this.nextSeq = null;
    this.targetMs = TARGET_MS;
    this.onUpdate = null;
    this.quiet = 0;
    this.stats = {
      packets: 0, decoded: 0, lost: 0, recovered: 0, late: 0,
      overflow: 0, underrun: 0, bufferMs: 0, stalled: true,
    };
  }

  async init() {
    if (this.ctx) {
      await this.ctx.resume().catch(() => {});
      return;
    }
    const Ctx = window.AudioContext || window.webkitAudioContext;
    if (!Ctx) throw new Error('当前浏览器不支持 AudioContext');
    /* 显式要 48kHz：与 Opus 原采样率一致，正常路径完全不需要重采样 */
    const ctx = new Ctx({ latencyHint: 'interactive', sampleRate: RATE });
    await ctx.audioWorklet.addModule('/src/audio-worklet.js');

    const node = new AudioWorkletNode(ctx, 'ww-audio', {
      numberOfInputs: 0,
      numberOfOutputs: 1,
      outputChannelCount: [CH],
      processorOptions: { channels: CH, targetMs: TARGET_MS },
    });
    node.port.onmessage = (e) => this.#onNode(e.data);

    const gain = ctx.createGain();
    gain.gain.value = this.#gain();
    node.connect(gain).connect(ctx.destination);

    const dec = new AudioDecoder({
      output: (d) => this.#onData(d),
      error: (e) => { this.error = e?.message || String(e); },
    });
    dec.configure({
      codec: 'opus',
      sampleRate: RATE,
      numberOfChannels: CH,
      description: opusHead(CH, RATE),
    });

    this.ctx = ctx;
    this.node = node;
    this.gain = gain;
    this.decoder = dec;
    this.ready = true;
    if (ctx.state !== 'running') await ctx.resume().catch(() => {});
  }

  /* ── 收到的音频包 ───────────────────────────────────────────── */
  push(buf) {
    if (!this.ready || !this.decoder || this.decoder.state !== 'configured') return;
    if (buf.byteLength < AUDIO_HDR_SIZE) return;

    const h = parseAudio(buf);
    if (h.version !== AUDIO_VERSION) return;

    const end = AUDIO_HDR_SIZE + h.len + h.redLen;
    if (buf.byteLength < end) { this.stats.late++; return; }

    const main = h.len ? buf.subarray(AUDIO_HDR_SIZE, AUDIO_HDR_SIZE + h.len) : null;
    const red = h.redLen ? buf.subarray(AUDIO_HDR_SIZE + h.len, end) : null;

    this.stats.packets++;

    if (this.nextSeq === null) {
      /* 首包只对齐序号：从下一个完整帧开始播，避免半截开头 */
      this.nextSeq = (h.seq + 1) & 0xffff;
      return;
    }

    const ahead = (h.seq - this.nextSeq + 0x10000) & 0xffff;
    if (ahead === 0) {
      this.#submit(main, h.seq);
      this.nextSeq = (h.seq + 1) & 0xffff;
      return;
    }
    if (ahead >= 0x8000) {
      this.stats.late++;   /* 序号回退：已播过的迟到包 */
      return;
    }

    /* 有缺口：丢了 ahead 个包。最近那个（seq-1）正好是本包 RED 携带的 */
    const samplePerPkt = Math.round((RATE * AUDIO_FRAME_MS) / 1000);
    if (ahead === 1 && red) {
      this.#submit(red, (h.seq - 1) & 0xffff);
      this.stats.recovered++;
    } else {
      this.stats.lost += ahead;
      this.node?.port.postMessage({ type: 'gap', frames: ahead * samplePerPkt });
    }
    this.#submit(main, h.seq);
    this.nextSeq = (h.seq + 1) & 0xffff;
  }

  #submit(data, seq) {
    if (!data || !data.length) return;
    try {
      /* Opus 每帧独立解码，统一按 key 提交 */
      this.decoder.decode(new EncodedAudioChunk({
        type: 'key',
        timestamp: seq * AUDIO_FRAME_MS * 1000,
        data,
      }));
    } catch {
      this.stats.late++;
    }
  }

  #onData(d) {
    try {
      const frames = d.numberOfFrames | 0;
      const nch = d.numberOfChannels;
      const srcRate = d.sampleRate;
      const ch0 = new Float32Array(frames);
      d.copyTo(ch0, { planeIndex: 0, format: 'f32-planar' });
      let ch1;
      if (nch > 1) {
        ch1 = new Float32Array(frames);
        d.copyTo(ch1, { planeIndex: 1, format: 'f32-planar' });
      } else {
        ch1 = ch0.slice();
      }

      const outRate = this.ctx ? this.ctx.sampleRate : RATE;
      const a = srcRate === outRate ? ch0 : this.#resample(ch0, srcRate, outRate);
      const b = srcRate === outRate ? ch1 : this.#resample(ch1, srcRate, outRate);

      this.node?.port.postMessage(
        { type: 'push', ch0: a, ch1: b, frames: a.length },
        [a.buffer, b.buffer],
      );
      this.stats.decoded += frames;
    } finally {
      try { d.close(); } catch { /* ignore */ }
    }
  }

  /* 仅在 AudioContext 拿不到 48kHz 时走这里（Chrome 正常不会），逐块线性插值 */
  #resample(src, from, to) {
    const n = Math.max(1, Math.round((src.length * to) / from));
    const out = new Float32Array(n);
    const step = (src.length - 1) / Math.max(1, n - 1);
    for (let i = 0; i < n; i++) {
      const p = i * step;
      const i0 = Math.floor(p);
      const f = p - i0;
      const x = src[i0];
      const y = src[i0 + 1] ?? x;
      out[i] = x + (y - x) * f;
    }
    return out;
  }

  #onNode(m) {
    if (!m) return;
    if (m.type === 'level') {
      this.stats.bufferMs = m.ms;
      this.stats.stalled = !!m.stalled;
      const un = m.underrun | 0;
      this.stats.underrun += un;
      /* 欠载才加水位（上限 120ms）；连续 10 个周期（1s）无欠载再慢慢回落 */
      if (un > 0 && this.targetMs < 120) {
        this.targetMs = Math.min(120, this.targetMs + 20);
        this.node?.port.postMessage({ type: 'target', ms: this.targetMs });
        this.quiet = 0;
      } else if (this.targetMs > TARGET_MS) {
        this.quiet = (this.quiet || 0) + 1;
        if (this.quiet >= 10) {
          this.quiet = 0;
          this.targetMs = Math.max(TARGET_MS, this.targetMs - 10);
          this.node?.port.postMessage({ type: 'target', ms: this.targetMs });
        }
      }
      this.onUpdate?.();
    } else if (m.type === 'overflow') {
      this.stats.overflow += m.drop | 0;
    }
  }

  /* ── 音量与静音（只作用于本机播放，不改目标机 sink）──────────── */
  #gain() {
    return this.muted ? 0 : Math.pow(this.volume, 2);   /* 二次曲线更接近听感 */
  }

  #applyGain() {
    if (!this.gain || !this.ctx) return;
    this.gain.gain.setTargetAtTime(this.#gain(), this.ctx.currentTime, 0.02);
  }

  setVolume(v) {
    this.volume = Math.max(0, Math.min(1, Number(v) || 0));
    this.#applyGain();
  }

  setMuted(on) {
    this.muted = !!on;
    this.#applyGain();
  }

  setTargetMs(ms) {
    this.targetMs = Math.max(TARGET_MS, Math.min(120, ms | 0));
    this.node?.port.postMessage({ type: 'target', ms: this.targetMs });
  }

  reset() {
    this.nextSeq = null;
    this.stats.bufferMs = 0;
    this.node?.port.postMessage({ type: 'reset' });
  }

  close() {
    try { this.decoder?.close(); } catch { /* ignore */ }
    try { this.ctx?.close(); } catch { /* ignore */ }
    this.decoder = null;
    this.node = null;
    this.gain = null;
    this.ctx = null;
    this.ready = false;
    this.nextSeq = null;
  }
}
