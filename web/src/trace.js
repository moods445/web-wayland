// 输入/画面追踪：常开的环形缓冲 + 延迟统计 + JSON 导出。
//
// 用途：鼠标"卡"这类问题靠肉眼说不清，需要把「输入发出 → 服务端收到 → 注入完成
// → 画面帧到达 → 解码 → 上屏」整条链路的时间戳记下来，事后按用户操作回放分析。
//
// 设计约束：
//   * 纯内存、无 IO、无定时器：记录动作只有一次 push，导出时才序列化，
//     因此可以常开（60Hz 输入 + 60fps 画面约 7200 事件/分钟）。
//   * 时间是 performance.now()（本机单调时钟），与服务端时钟通过消息里的 ts 字段对齐。

const CAP = 60000;        // 事件上限，超出按 1/8 批量丢弃最旧的，避免逐条 shift
const TRACK = 200;        // 待观测反馈的输入上限
const MAX_LAG = 4000;     // 延迟样本上限

export class Trace {
  constructor() {
    this.ev = [];         // {k,t,...}
    this.dropped = 0;
    this.pending = [];    // 已发出、还在等可见反馈的指针输入
    this.lags = [];       // 输入 → 首个上屏帧的延迟（ms）
    this.inputs = 0;
    this.frames = 0;
    this.pairs = [];      // {t, lag}：逐条输入的 input→上屏 延迟，导出后与服务端按 ts 对齐
    this.gaps = 0;        // 帧到达间隔 > 50ms 的次数（掉帧/卡顿）
    this.maxGap = 0;
    this.lastFrameAt = 0;
    this.startedAt = performance.now();
    this.onSpike = null;   // (lagMs) => void：单次输入延迟超阈值时回调（自动上传用）
  }

  #push(e) {
    const ev = this.ev;
    if (ev.length >= CAP) {
      const cut = CAP >> 3;
      ev.splice(0, cut);
      this.dropped += cut;
    }
    ev.push(e);
  }

  /* 出站输入：给消息补一个客户端时间戳，返回同一个对象（调用方直接 send） */
  input(m) {
    const t = performance.now();
    m.ts = t;
    this.inputs++;
    this.#push({ k: m.t, t, x: m.x, y: m.y, v: m.v, k2: m.k, d: m.d });
    if (m.t === 'pm' || m.t === 'pr' || m.t === 'pb') {
      this.pending.push({ t, k: m.t, x: m.x, y: m.y });
      if (this.pending.length > TRACK) this.pending.shift();
    }
    return m;
  }

  /* 一帧分片重组完成（进入解码前） */
  frame(c) {
    const t = c.arrivedAt ?? performance.now();
    this.frames++;
    if (this.lastFrameAt) {
      const g = t - this.lastFrameAt;
      if (g > 50) { this.gaps++; if (g > this.maxGap) this.maxGap = g; }
    }
    this.lastFrameAt = t;
    this.#push({ k: 'frm', t, id: c.id, pts: c.pts, key: c.key ? 1 : 0, n: c.bytes });
  }

  /* 解码出帧（尚未 drawImage） */
  decoded(t, pts) {
    this.#push({ k: 'dec', t, pts });
  }

  /* 上屏完成：结算所有早于此刻的输入（取首个上屏帧作为可见反馈） */
  rendered(t, pts) {
    this.#push({ k: 'ren', t, pts });
    if (!this.pending.length) return;
    let n = 0;
    while (this.pending.length && this.pending[0].t <= t) {
      const i = this.pending.shift();
      const lag = t - i.t;
      this.lags.push(lag);
      this.pairs.push({ t: +i.t.toFixed(3), lag: +lag.toFixed(2) });
      if (lag > 120) this.onSpike?.(lag);
      n++;
    }
    if (n && this.lags.length > MAX_LAG) {
      this.lags.splice(0, MAX_LAG >> 2);
      if (this.pairs.length > MAX_LAG) this.pairs.splice(0, MAX_LAG >> 2);
    }
  }

  #percentiles(arr) {
    if (!arr.length) return null;
    const s = arr.slice().sort((a, b) => a - b);
    const at = (p) => s[Math.min(s.length - 1, Math.floor((s.length - 1) * p))];
    return {
      n: s.length,
      p50: +at(0.5).toFixed(1),
      p95: +at(0.95).toFixed(1),
      p99: +at(0.99).toFixed(1),
      max: +s[s.length - 1].toFixed(1),
      avg: +(s.reduce((a, b) => a + b, 0) / s.length).toFixed(1),
    };
  }

  stats() {
    const dur = (performance.now() - this.startedAt) / 1000;
    return {
      durSec: +dur.toFixed(1),
      events: this.ev.length,
      dropped: this.dropped,
      inputs: this.inputs,
      frames: this.frames,
      inputRate: +(this.inputs / Math.max(dur, 1e-3)).toFixed(1),
      frameRate: +(this.frames / Math.max(dur, 1e-3)).toFixed(1),
      inputLag: this.#percentiles(this.lags),
      frameGapsOver50ms: this.gaps,
      maxFrameGapMs: +this.maxGap.toFixed(1),
      pairs: this.pairs.length,
    };
  }

  /* 导出：stats + 原始事件（按时间升序）+ 元信息，供 scripts/trace-report.mjs 分析 */
  dump(meta = {}) {
    return {
      v: 1,
      side: 'client',
      meta: { ...meta, ua: navigator.userAgent },
      stats: this.stats(),
      pairs: this.pairs.slice(),
      events: this.ev.slice(),
    };
  }

  /* NDJSON 追加流：一行一条记录（snap / ev / pair），网关原样 append。
     行式结构的好处：可以 tail -f 看实时、可以逐行 jq、单行不会被撑到上百 KB
     （之前每次重写一整个 JSON 对象，一次上传就是一行 100KB+ 的大块）。
     上送的是滚动窗口（最近 nEv 条事件 / nPair 条延迟样本），
     所以任意一个 snap 块都自带"刚刚卡的那一下"，不依赖前后切片。 */
  ndjson(meta = {}, windowMs = 30000, maxEv = 8000) {
    const now = performance.now();
    const cut = now - windowMs;
    const out = [];
    /* 按时间窗口切，而不是按条数：frm/dec/ren 每帧产生 3 条，
       2000 条只覆盖 ~12 秒，而且指针事件稀疏，很容易整段被挤出去（曾经的坑：
       文件里一条 pm 都没有，导致"数据看起来很好"但用户实际很卡）。 */
    let ev = this.ev;
    if (ev.length && ev[0].t < cut) {
      let i = 0;
      while (i < ev.length && ev[i].t < cut) i++;
      ev = ev.slice(i);
    }
    if (ev.length > maxEv) ev = ev.slice(-maxEv);
    let pairs = this.pairs;
    if (pairs.length && pairs[0].t < cut) {
      let i = 0;
      while (i < pairs.length && pairs[i].t < cut) i++;
      pairs = pairs.slice(i);
    }
    out.push(JSON.stringify({
      type: 'snap',
      takenAt: new Date().toISOString(),
      meta: { ...meta, ua: navigator.userAgent },
      stats: this.stats(),
      window: { ms: windowMs, events: ev.length, pairs: pairs.length },
    }));
    for (const e of ev) {
      /* frm/dec/ren 带上 id / pts / 字节数：pts 是服务端抓帧时间戳，
         用它才能把"画面里的光标坐标"和客户端这一帧对上（光标可见延迟）。 */
      out.push(JSON.stringify({
        type: 'ev', kind: e.k, t: e.t,
        x: e.x, y: e.y, v: e.v, key: e.k2, down: e.d,
        id: e.id, pts: e.pts, n: e.n,
      }));
    }
    for (const p of pairs) {
      out.push(JSON.stringify({ type: 'pair', t: p.t, lag: p.lag }));
    }
    return out.join('\n') + '\n';
  }

  reset() {
    this.ev.length = 0;
    this.pending.length = 0;
    this.lags.length = 0;
    this.inputs = this.frames = this.gaps = 0;
    this.dropped = 0;
    this.maxGap = 0;
    this.lastFrameAt = 0;
    this.startedAt = performance.now();
  }
}

/* 自动上传器：把追踪窗口 POST 给网关，用户不需要手动导出。
 *
 * 两份触发：
 *   1. 定时（默认 15s）——保证"事后想看"时服务端总有最近的数据；
 *   2. 突刺（单次输入延迟 > 120ms，5 秒内只发一次）——卡的那一下立刻被抓住。
 * 单份快照自带最近 2500 条事件（约 20~25 秒），所以任意一份都够回放，
 * 不需要靠时间序把多份拼起来。
 * 上传永远不阻塞渲染/输入：失败静默，且同一时刻只允许一个请求在飞。 */
export class TraceUploader {
  constructor(trace, { url = '/trace-upload', intervalMs = 15000, meta = () => ({}) } = {}) {
    this.trace = trace;
    this.url = url;
    this.intervalMs = intervalMs;
    this.meta = meta;
    this.timer = 0;
    this.inFlight = false;
    this.lastSpikeAt = 0;
    this.sent = 0;
    this.failures = 0;
    this.bytes = 0;
    this.lastOkAt = 0;
    this.lastError = null;
  }

  start() {
    if (this.timer) return;
    this.trace.onSpike = (ms) => this.spike(ms);
    this.flush('start');
    this.timer = setInterval(() => this.flush('tick'), this.intervalMs);
  }

  stop() {
    if (this.timer) {
      clearInterval(this.timer);
      this.timer = 0;
    }
    this.flush('stop');
  }

  spike(lagMs) {
    const now = performance.now();
    if (now - this.lastSpikeAt < 5000) return;
    this.lastSpikeAt = now;
    this.flush(`spike ${Math.round(lagMs)}ms`);
  }

  async flush(reason = 'manual') {
    if (this.inFlight) return;      // 上一次没回来就跳过，绝不排队堆积
    if (!this.trace.ev.length && !this.trace.pairs.length) return;   // 还没操作过，别传空包
    this.inFlight = true;
    try {
      const body = this.trace.ndjson({ ...this.meta(), reason });
      const r = await fetch(this.url, {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-ndjson' },
        body,
        /* keepalive 有 64KB 上限（超出直接 Failed to fetch），
           所以只在小包（如最后一发）时启用；正常快照走普通请求。 */
        keepalive: body.length < 60000,
      });
      if (r.ok) {
        this.sent++;
        this.bytes += body.length;
        this.lastOkAt = performance.now();
      } else {
        this.failures++;
        this.lastError = `HTTP ${r.status}`;
      }
    } catch (e) {
      this.failures++;
      this.lastError = e?.message ?? 'network';
    } finally {
      this.inFlight = false;
    }
  }

  stat() {
    return { sent: this.sent, failures: this.failures, bytes: this.bytes, lastError: this.lastError };
  }
}
