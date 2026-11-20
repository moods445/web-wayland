// web-wayland 前端主控
import { Transport } from './transport.js';
import { Decoder, codecFromAnnexB } from './decoder.js';
import { AudioPlayer } from './audio.js';
import { Input } from './input.js';
import { Trace, TraceUploader } from './trace.js';

/* 输入/画面追踪：常开、纯内存，导出时可回放分析「鼠标为什么卡」 */
const trace = new Trace();

/* 追踪自动上传：定时 + 延迟突刺时把最近窗口 POST 给网关，用户无需手动导出。
   meta() 在每次上传时才取值，所以可以引用后面才声明的模块变量。 */
const traceUp = new TraceUploader(trace, {
  meta: () => ({
    resolution,
    strategy: Number(pref.strategy),
    fps: pref.fps,
    enc: pref.enc,
    bitrate: pref.bitrate,
    chroma: pref.chroma,
    status: {
      e2eMs: Number((e2e || 0).toFixed(1)),
      queue: decoder?.queue ?? 0,
      rttMs: rtts.length ? Number((rtts.reduce((a, b) => a + b, 0) / rtts.length).toFixed(1)) : null,
      decodeMs: decoder?.decodeMs == null ? null : Number(decoder.decodeMs.toFixed(2)),
      renderMs: renderStats.n ? Number((renderStats.sum / renderStats.n).toFixed(2)) : null,
    },
  }),
});

const $ = (id) => document.getElementById(id);

const ui = {
  connect: $('connect'), stage: $('stage'), canvas: $('screen'),
  hud: $('hud'),
  enter: $('enter'), enterFs: $('enter-fs'), steps: $('steps'), errors: $('errors'),
  addr: $('server-addr'), copy: $('copy-addr'),
  bitrate: $('hud-bitrate'), bitrateTarget: $('hud-bitrate-target'),
  fps: $('hud-fps'), res: $('hud-res'),
  latency: $('hud-latency'), tier: $('hud-tier'),
  cursor: $('cursor-local'),
  gear: $('gear'), settings: $('settings'), setRes: $('set-res'), setResNote: $('set-res-note'),
  setFps: $('set-fps'), setEnc: $('set-enc'),
  setFit: $('set-fit'), setHud: $('set-hud'), setClose: $('set-close'), setNote: $('set-note'),
  setQp: $('set-qp'), setQpOut: $('set-qp-out'), setQpRow: $('set-qp-row'),
  setBitrate: $('set-bitrate'), setBitrateOut: $('set-bitrate-out'),
  setBitrateNote: $('set-bitrate-note'), setBitrateRow: $('set-bitrate-row'),
  setMode: $('set-mode'), setDecode: $('set-decode'),
  setStrategy: $('set-strategy'), setStrategyOut: $('set-strategy-out'), setStrategyNote: $('set-strategy-note'),
  setChroma: $('set-chroma'), setChromaNote: $('set-chroma-note'),
  setChromaRow: $('set-chroma-row'), setChromaHint: $('set-chroma-hint'),
  setQuality: $('set-quality'), warn: $('warn'),
  setVol: $('set-vol'), setVolOut: $('set-vol-out'),
  setMuteHost: $('set-mute-host'), setMuteBlur: $('set-mute-blur'), setAudioState: $('set-audio-state'),
  btnRestart: $('btn-restart'), btnRestartDeep: $('btn-restart-deep'), setRestartNote: $('set-restart-note'),
  btnTrace: $('btn-trace'), btnTraceReset: $('btn-trace-reset'), setTraceOut: $('set-trace-out'),
  setStats: $('set-stats'), stats: $('stats'),
  setHelp: $('set-help'), help: $('help'), helpClose: $('help-close'),
  report: $('report'), reportList: $('report-list'), reportClose: $('report-close'),
};

/* ── 客户端串流设置（只影响本机这条连接）──────────────────────
   分辨率/帧率是「请求」：服务端按档位与上限执行，并在自动模式下按实测帧率升降档。
   手动选分辨率会带 lock=1，关闭服务端的自动升降档，避免和用户选择打架。
   画质滑块本质是 QP：本机 H.264 只有 CQP，没有真正的码率控制。 */
const pref = Object.assign({ res: 'auto', fps: 0, hud: true, enc: '', fit: 'fill',
  /* strategy: 0 平衡 1 流畅 2 画质 3 自定义（手动 QP）；chroma: 4:4:4 偏好
     bitrate: 目标码率（Mbps），0 = 关闭、按 QP 编码。只有支持 CBR 的编码器（NVENC）生效 */
  qp: 20, strategy: 0, bitrate: 0, chroma: false, decode: 'auto', mode: 'window', quality: true, stats: false,
  /* 音频：vol 只作用于本机播放；muteHost 是「静音目标机扬声器」；muteBlur 失焦静音 */
  vol: 100, muteHost: false, muteBlur: false },
  JSON.parse(localStorage.getItem('ww.pref') || '{}'));
/* 老版本把 enc 默认写成了 vaapi，会导致连接时无条件切换服务端编码器；
   空串表示“跟随服务端启动参数”，只有用户手动选过才下发。 */
if (pref.enc === 'vaapi') pref.enc = '';
const savePref = () => localStorage.setItem('ww.pref', JSON.stringify(pref));
let manualRes = null;   // 非 auto 时禁止自动跟随窗口

/* 画质滑块 0~100（右=更清晰）↔ QP 30~16（小=更清晰） */
const QP_MIN = 16, QP_MAX = 30;
const qpToSlider = (qp) => Math.round(((QP_MAX - qp) / (QP_MAX - QP_MIN)) * 100);
const sliderToQp = (v) => Math.round(QP_MAX - (Number(v) / 100) * (QP_MAX - QP_MIN));

/* 三种策略只改 QP 与目标帧率，分辨率始终由下拉决定；4:4:4 是独立开关 */
const STRATEGY_QP = { 0: 20, 1: 26, 2: 16 };
const STRATEGY_LABEL = { 0: '平衡', 1: '流畅优先', 2: '画质优先', 3: '自定义' };
const STRATEGY_NOTE = {
  0: '平衡：QP 20，兼顾清晰与流畅。',
  1: '流畅优先：目标 60/30 fps，QP 24~30 按实测帧率自动浮动。',
  2: '画质优先：QP 16 固定，不设帧率目标；4K 下受硬件限制可能约 20 fps。',
  3: '自定义：手动调整画质滑块后进入，策略不再自动改 QP。',
};

/* ── 统计信息面板（独立于状态叠层，默认关闭）──────────────── */
let codecName = '—';
let chromaName = '—';   // 从 SPS profile 推断实际生效的色度采样
let activeCodec = null; // 当前解码器配置的 codec 串
const renderStats = { sum: 0, n: 0 };   // drawImage 绘制耗时
const rtts = [];                        // 最近 30 次 ping/pong
const rate = { t: 0, inFrames: 0, decoded: 0, rendered: 0, inFps: 0, decFps: 0, renFps: 0 };
const statRows = new Map();

function codecLabel(codec) {
  const profile = parseInt(codec.slice(5, 7), 16);
  const name = profile === 0x64 ? 'H.264 High'
    : profile === 0xf4 ? 'H.264 High 4:4:4'
      : profile === 0x4d ? 'H.264 Main'
        : profile === 0x42 ? 'H.264 Baseline' : 'H.264';
  return `${name} 8-bit SDR`;
}

function buildStats() {
  const defs = [
    ['stream', 'Video stream', true],
    ['bitrate', 'Video bitrate (actual / target)'],
    ['in', 'Incoming frame rate from network'],
    ['dec', 'Decoding frame rate'],
    ['ren', 'Rendering frame rate'],
    ['host', 'Host processing latency min/max/average'],
    ['dropNet', 'Frames dropped by your network connection'],
    ['dropJit', 'Frames dropped due to network jitter'],
    ['rtt', 'Average network latency'],
    ['decodeMs', 'Average decoding time'],
    ['queueMs', 'Average frame queue delay'],
    ['renderMs', 'Average rendering time'],
    ['inputLag', 'Input → screen latency p50 / p95 / max'],
  ];
  const head = document.createElement('div');
  head.className = 'stats-title';
  head.innerHTML = '<span>STATISTICS</span><span class="stats-dot"></span>';
  ui.stats.appendChild(head);
  for (const [key, label, stack] of defs) {
    const row = document.createElement('div');
    row.className = `stat-row${stack ? ' stack' : ''}`;
    const dt = document.createElement('span');
    dt.className = 'stat-label';
    dt.textContent = label;
    const dd = document.createElement('span');
    dd.className = 'stat-value';
    dd.textContent = '-';
    row.append(dt, dd);
    ui.stats.appendChild(row);
    statRows.set(key, dd);
  }
}
buildStats();

function setStatsVisible(on) {
  pref.stats = !!on;
  ui.setStats.checked = pref.stats;
  ui.stats.classList.toggle('hidden', !pref.stats);
  savePref();
  if (pref.stats) {
    rate.t = 0;          // 重新开表时丢弃隐藏期间的间隔
    rtts.length = 0;
    if (transport && !transport.closed) transport.send({ t: 'ping', ts: performance.now() });
  }
}

function setHudVisible(on) {
  pref.hud = !!on;
  ui.setHud.checked = pref.hud;
  ui.hud.classList.toggle('hidden', !pref.hud);
  savePref();
}

function setHelpVisible(on) {
  ui.help.classList.toggle('hidden', !on);
}

/* 每秒刷新；不可测的字段显示 -（见快捷键帮助里的字段说明） */
function updateStats() {
  if (ui.stats.classList.contains('hidden')) return;
  const now = performance.now();
  const st = transport?.stats;
  if (!rate.t) {
    rate.t = now;
    rate.inFrames = st?.frames ?? 0;
    rate.decoded = decoder?.decoded ?? 0;
    rate.rendered = rendered;
    return;
  }
  const dt = Math.max(0.2, (now - rate.t) / 1000);
  const set = (k, text) => { const el = statRows.get(k); if (el) el.textContent = text; };

  const inFrames = st?.frames ?? 0;
  const decoded = decoder?.decoded ?? 0;
  rate.inFps = (inFrames - rate.inFrames) / dt;
  rate.decFps = (decoded - rate.decoded) / dt;
  rate.renFps = (rendered - rate.rendered) / dt;
  rate.t = now;
  rate.inFrames = inFrames;
  rate.decoded = decoded;
  rate.rendered = rendered;

  set('stream', `${resolution} ${rate.renFps.toFixed(2)} FPS (Codec: ${codecName} · ${chromaName})`);
  set('bitrate', `${lastMbps.toFixed(1)} Mbps / 目标 ${targetBitrateLabel()}`);
  set('in', `${rate.inFps.toFixed(2)} FPS`);
  set('dec', `${rate.decFps.toFixed(2)} FPS`);
  set('ren', `${rate.renFps.toFixed(2)} FPS`);
  /* 协议头只有 pts_us（主机时钟），没有采集/编码时间戳，跨机不可比 */
  set('host', '- / - / - ms');

  if (st) {
    const netTotal = st.frames + st.incomplete;
    const jitTotal = st.frames + st.late;
    set('dropNet', `${netTotal ? ((st.incomplete / netTotal) * 100).toFixed(2) : '0.00'}%`);
    set('dropJit', `${jitTotal ? ((st.late / jitTotal) * 100).toFixed(2) : '0.00'}%`);
  } else {
    set('dropNet', '-');
    set('dropJit', '-');
  }

  if (rtts.length) {
    const avg = rtts.reduce((a, b) => a + b, 0) / rtts.length;
    const variance = Math.sqrt(rtts.reduce((a, b) => a + (b - avg) ** 2, 0) / rtts.length);
    set('rtt', `${avg.toFixed(1)} ms (variance: ${variance.toFixed(1)} ms)`);
  } else {
    set('rtt', '- (variance: -)');
  }

  const dm = decoder?.decodeMs;
  const qm = decoder?.queueMs;
  set('decodeMs', dm == null ? '-' : `${dm.toFixed(2)} ms`);
  set('queueMs', qm == null ? '-' : `${qm.toFixed(2)} ms`);
  set('renderMs', renderStats.n ? `${(renderStats.sum / renderStats.n).toFixed(2)} ms` : '-');
  {
    const l = trace.stats();
    const g = l.inputLag;
    set('inputLag', g
      ? `${g.p50} / ${g.p95} / ${g.max} ms  (n=${g.n}, 帧间隔>50ms ${l.frameGapsOver50ms} 次)`
      : '-');
  }

  /* 应用层 RTT：网关直接回 pong，不打扰 wwcap */
  if (transport && !transport.closed) transport.send({ t: 'ping', ts: now });
}

function onCtrl(msg) {
  /* 重启流服务的服务端回执（出没出图由客户端按帧计数确认，见 requestRestart） */
  if (msg?.t === 'restart') {
    if (msg.stage === 'busy') setRestartNote('刚重启过，等 5 秒再试', 'warn');
    else if (msg.stage === 'starting') {
      setRestartNote(msg.deep ? '服务端已接受，正在重启桌面会话…' : '服务端已接受，正在重启采集…', 'warn');
    }
    return;
  }
  if (msg?.t !== 'pong') return;
  const rtt = performance.now() - Number(msg.ts);
  if (Number.isFinite(rtt) && rtt >= 0 && rtt < 10000) {
    rtts.push(rtt);
    if (rtts.length > 30) rtts.shift();
  }
}

function requestRes(val) {
  if (val === 'auto') {
    manualRes = null;
    autoMode();
    return;
  }
  const w = Number(val.split('x')[0]);
  const h = Math.round((w * 9) / 16);
  const logical = window.innerWidth || w;
  const scale = Math.min(2, Math.max(1, w / logical));
  manualRes = val;
  requestedMode = { w, h, scale, lock: 1 };
  transport?.send({ t: 'mode', w, h, scale, lock: 1 });
  console.log('ww: 手动分辨率', w, h, 'scale', scale);
}

/* 自动档：按窗口尺寸 × DPR 请求，由服务端决定最终档位。
   服务端固定按 16:9 档位（1920/2560/3200/3840）出图，requestedMode 记录
   同样的档位结果，供自测断言使用，避免拿窗口原始高度（非 16:9）去比对。 */
function autoMode() {
  const dpr = Math.min(window.devicePixelRatio || 1, 2);
  const reqW = Math.min(Math.round(window.innerWidth * dpr), 3840);
  const reqH = Math.min(Math.round(window.innerHeight * dpr), 2160);
  /* 必须和服务端保持同一套“向下取档”规则：
     服务端是 >=2560 给 2560、>=3200 给 3200、>=3840 给 3840，
     这里若用“就近取档”，HUD/日志会显示 3200 而实际拿到 2560，排障时误导。 */
  let w = 1920;
  if (reqW >= 2560) w = 2560;
  if (reqW >= 3200) w = 3200;
  if (reqW >= 3840) w = 3840;
  const h = (w * 9 / 16) & ~1;
  requestedMode = { w, h, scale: dpr };
  transport?.send({ t: 'mode', w: reqW, h: reqH, scale: dpr, lock: 0 });
  console.log('ww: auto mode', reqW, reqH, 'scale', dpr, '→ 档位', w, h);
}

function requestFps(v) {
  transport?.send({ t: 'fps', v: Number(v) || 0 });
}

function requestQp(v) {
  transport?.send({ t: 'qp', v: Number(v) });
}

function requestBitrate(mbps) {
  transport?.send({ t: 'bitrate', v: Math.round(Number(mbps) * 1e6) });
}

/* 画质滑块只表示 QP；实际码率统一的统计信息里看，避免两处数字含义不同 */
function renderQp() {
  ui.setQpOut.textContent = `QP ${pref.qp}`;
}

/* 目标码率：仅支持 CBR 的编码器（NVENC）可设；不支持时置灰并说明原因 */
function renderBitrate() {
  const cbr = caps.rc === 'cbr';
  const v = Math.max(0, Math.min(80, Number(pref.bitrate) || 0));
  ui.setBitrate.value = String(cbr ? v : 0);
  ui.setBitrate.disabled = !cbr;
  if (cbr) {
    ui.setBitrateOut.textContent = v > 0 ? `${v} Mbps` : '关闭（QP 控制）';
    ui.setBitrateNote.textContent = v > 0
      ? `按 ${v} Mbps 恒定输出（CBR）：静态画面用填充占满，实时值见统计信息。`
      : '0 = 关闭，按画质（QP）编码；拖到右侧按目标码率恒定输出。';
  } else {
    ui.setBitrateOut.textContent = capsLoaded ? '不可用（仅 CQP）' : '读取中…';
    ui.setBitrateNote.textContent = capsLoaded
      ? '当前编码器只支持 CQP，码率随画面复杂度浮动；切到独显 NVENC 后可按目标码率编码。'
      : '正在读取编码器能力…';
  }
  ui.setBitrateRow.classList.toggle('muted', !cbr);
  /* 目标码率生效时 QP 不参与码控，置灰画质滑块避免“拖了没反应” */
  const qpLocked = cbr && v > 0;
  ui.setQp.disabled = qpLocked;
  ui.setQpRow.classList.toggle('muted', qpLocked);
}

/* 分辨率过高警示：集显（非 nvenc）下 3200/4K 实测只有 20~30fps，
   而且客户端解码 4K 也会吃掉上百毫秒，表现为"鼠标几乎用不了"。 */
function renderResNote() {
  const note = ui.setResNote;
  if (!note) return;
  const v = ui.setRes.value;
  const high = v === '3200x1800' || v === '3840x2160';
  const igpu = caps.encoder && caps.encoder !== 'nvenc';
  if (high && igpu) {
    note.textContent = `当前编码器是集显（${caps.encoder}），该分辨率实测只有 20~30fps、延迟上百毫秒，鼠标会明显发卡；建议 1920 × 1080。`;
    note.classList.remove('hidden');
  } else {
    note.textContent = '';
    note.classList.add('hidden');
  }
}

/* ── 本地光标 ─────────────────────────────────────────────────
 * 服务端用全透明光标主题后，画面里不再有光标（/capabilities 回报 cursor=local），
 * 这里自己画一个：位置直接取系统指针的 clientX/clientY，不走视频链路，
 * 所以延迟是 0，也不受帧率/丢帧影响。光标形状固定为箭头（代价已知）。 */
let localCursor = false;

function enableLocalCursor(on) {
  localCursor = !!on;
  if (ui.canvas) ui.canvas.style.cursor = on ? 'none' : '';
  if (!on && ui.cursor) ui.cursor.classList.add('hidden');
}

function placeLocalCursor(clientX, clientY) {
  if (!localCursor || !ui.cursor) return;
  if (clientX == null) {          // 指针离开画布 → 藏起来，交还给系统光标
    ui.cursor.classList.add('hidden');
    return;
  }
  ui.cursor.style.transform = `translate3d(${clientX}px, ${clientY}px, 0)`;
  ui.cursor.classList.remove('hidden');
}

/* 指针离开窗口时本地光标要一起藏掉，否则会停在边缘 */
document.addEventListener('pointerleave', () => {
  if (localCursor && ui.cursor) ui.cursor.classList.add('hidden');
});

/* 统计面板/叠层里显示的“设置码率”：区分「没设」与「编码器不支持」 */
function targetBitrateLabel() {
  if (caps.rc !== 'cbr') return '不可用（仅 CQP）';
  return pref.bitrate > 0 ? `${pref.bitrate} Mbps` : '关闭（QP 控制）';
}

/* ── 输入追踪导出 ─────────────────────────────────────────────
 * 客户端常开记录（trace.js），服务端每 5 秒把「收到→注入」耗时写进
 * /tmp/wwcap-trace.json 并由网关 /trace 暴露；导出时两份合并成一个文件，
 * 交给 scripts/trace-report.mjs 出报告。 */
async function collectTrace() {
  transport?.send({ t: 'trace', v: 1 });      // 让 wwcap 立刻落盘一次
  await new Promise((r) => setTimeout(r, 250));
  let server = null;
  try {
    const r = await fetch('/trace', { cache: 'no-store' });
    if (r.ok) {
      /* 服务端也是 NDJSON：逐行解析成 inputs / frames */
      const inputs = [];
      const frames = [];
      for (const line of (await r.text()).split('\n')) {
        if (!line) continue;
        try {
          const o = JSON.parse(line);
          if (o.type === 'in') inputs.push(o);
          else if (o.type === 'frm') frames.push(o);
        } catch { /* 跳过被轮转截断的残行 */ }
      }
      server = { v: 1, side: 'server', inputs, frames };
    }
  } catch { /* 服务端没就绪不影响客户端部分 */ }
  return {
    v: 1,
    takenAt: new Date().toISOString(),
    client: trace.dump({
      resolution, strategy: pref.strategy, fps: pref.fps,
      enc: pref.enc, chroma: pref.chroma, bitrate: pref.bitrate,
      status: window.__ww?.status?.(),
    }),
    server,
  };
}

function downloadJson(obj, name) {
  const blob = new Blob([JSON.stringify(obj)], { type: 'application/json' });
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 5000);
}

async function exportTrace() {
  if (ui.setTraceOut) ui.setTraceOut.textContent = '正在收集…';
  const data = await collectTrace();
  downloadJson(data, `ww-trace-${new Date().toISOString().replace(/[:.]/g, '-')}.json`);
  const lag = data.client.stats.inputLag;
  if (ui.setTraceOut) {
    ui.setTraceOut.textContent = lag
      ? `已导出 p50 ${lag.p50} / p95 ${lag.p95} ms`
      : '已导出（还没有指针操作）';
  }
  return data;
}

/* ── 串流策略与 4:4:4 能力 ────────────────────────────────────── */
/* rc: "cbr" = 可按目标码率编码，"cqp" = 码率随画面浮动（集显） */
/* cursor: "local" = 服务端已隐藏合成器光标，客户端自己画；"server" = 光标在视频里 */
let caps = { chroma444: false, encoder: '', rc: 'cqp', cursor: 'server' };
let capsLoaded = false;   // 拿到服务端能力前先乐观放行，避免误置灰
let decode444 = null;     // 浏览器能否解 H.264 High 4:4:4

async function probeDecode444() {
  if (!('VideoDecoder' in window)) {
    decode444 = false;
    return;
  }
  try {
    const r = await VideoDecoder.isConfigSupported({
      codec: 'avc1.f40028',
      avc: { format: 'annexb' },
      codedWidth: 1920,
      codedHeight: 1080,
    });
    decode444 = !!r.supported;
  } catch {
    decode444 = false;
  }
}

async function refreshCaps() {
  try {
    const r = await fetch('/capabilities', { cache: 'no-store' });
    if (r.ok) {
      caps = await r.json();
      capsLoaded = true;
    }
  } catch { /* 网关未提供 /capabilities 时保持乐观 */ }
  renderChroma();
  renderBitrate();
  renderResNote();
  /* 服务端隐藏了光标才启用本地光标，否则会双光标 */
  enableLocalCursor(caps.cursor === 'local');
}

function renderStrategy() {
  const s = Number(pref.strategy);
  ui.setStrategyOut.textContent = STRATEGY_LABEL[s] ?? '自定义';
  ui.setStrategyNote.textContent = STRATEGY_NOTE[s] ?? '';
  for (const b of ui.setStrategy.querySelectorAll('button[data-strategy]')) {
    b.classList.toggle('active', Number(b.dataset.strategy) === s);
  }
  /* 预设策略的 QP 与滑块保持一致（旧版本可能残留不同值） */
  if (s !== 3 && STRATEGY_QP[s] != null) {
    pref.qp = STRATEGY_QP[s];
    ui.setQp.value = String(qpToSlider(pref.qp));
    renderQp();
  }

  /* 流畅优先只留 30/60；其余策略保持 自动/30/45/60 */
  const opts = s === 1
    ? [[60, '60 fps'], [30, '30 fps']]
    : [[0, '自动'], [30, '30 fps'], [45, '45 fps'], [60, '60 fps']];
  if (!opts.some(([v]) => v === Number(pref.fps))) {
    pref.fps = opts[0][0];
    savePref();
    requestFps(pref.fps);
  }
  ui.setFps.innerHTML = opts
    .map(([v, label]) => `<option value="${v}">${label}</option>`)
    .join('');
  ui.setFps.value = String(pref.fps);
}

function renderChroma() {
  const enc = pref.enc || caps.encoder || '';
  let reason = '';
  if (enc === 'vaapi') {
    reason = '集显 VAAPI 不支持';
  } else if (!capsLoaded) {
    reason = decode444 === false ? '当前浏览器不支持 4:4:4 解码' : '';
  } else if (!caps.chroma444) {
    reason = '当前编码器/显卡不支持';
  } else if (decode444 === false) {
    reason = '当前浏览器不支持 4:4:4 解码';
  }
  const disabled = !!reason;
  ui.setChroma.disabled = disabled;
  ui.setChroma.checked = !!pref.chroma;
  ui.setChromaNote.textContent = reason ? `（${reason}）` : '';
  ui.setChromaRow.classList.toggle('muted', disabled);
  ui.setChromaHint.textContent = pref.chroma && disabled
    ? `已切回 4:2:0；${reason}，切回支持的编码器后自动恢复。`
    : '每个像素独立颜色信息，文字/红蓝边缘更锐利；码率约 +30~50%。';
}

function renderVol() {
  ui.setVolOut.textContent = `${pref.vol}%`;
}

function applyMode(mode) {
  try {
    if (mode === 'fullscreen') {
      document.documentElement.requestFullscreen?.()?.catch(() => {});
    } else if (document.fullscreenElement) {
      document.exitFullscreen?.()?.catch(() => {});
    }
  } catch { /* 无用户手势时浏览器会拒绝，不阻断串流 */ }
}

const host = location.hostname || '192.168.0.109';
const port = location.port || '8443';
ui.addr.textContent = `${host}:${port}`;

ui.copy.addEventListener('click', async () => {
  try { await navigator.clipboard.writeText(`${host}:${port}`); ui.copy.textContent = '已复制'; }
  catch { ui.copy.textContent = '复制失败'; }
  setTimeout(() => (ui.copy.textContent = '复制'), 1200);
});

function setStep(n, state) {
  [...ui.steps.children].forEach((li, i) => {
    li.classList.toggle('active', i === n && state === 'active');
    li.classList.toggle('done', i < n || (i === n && state === 'done'));
  });
}

function showErrors(list) {
  ui.errors.innerHTML = '';
  for (const t of list) {
    const li = document.createElement('li');
    li.textContent = t;
    ui.errors.appendChild(li);
  }
  ui.errors.classList.toggle('hidden', list.length === 0);
}

// 调试计数器：仅在 ?debug=1 时显示
const DEBUG = new URLSearchParams(location.search).has('debug');
const dbgEl = document.createElement('div');
dbgEl.style.cssText = 'position:fixed;left:12px;bottom:10px;z-index:40;font:11px ui-monospace,monospace;color:#7cf;opacity:.85;background:rgba(0,0,0,.5);padding:4px 8px;border-radius:6px';
dbgEl.textContent = 'boot';
if (DEBUG) ui.stage.appendChild(dbgEl);

/* desynchronized（低延迟直通）绕过合成器直接上屏，理论上省一帧；
   但在 macOS 上它可能脱离 vsync，表现为"帧率够但画面一顿一顿"。
   用 ?ds=0 可关掉做 A/B（上下文属性是创建时固定的，所以只能靠 URL 切换）。 */
const DESYNC = !new URLSearchParams(location.search).has('ds') ||
  new URLSearchParams(location.search).get('ds') !== '0';
const ctx = ui.canvas.getContext('2d', { alpha: false, desynchronized: DESYNC });
let transport = null, decoder = null, input = null, audio = null;
let rendered = 0, lastHudAt = 0, lastRenderedTs = 0, e2e = 0, resolution = '—', chunkLogged = false;
let lastMbps = 0;          // 上一秒实测码率，画质滑块旁展示
let lastArrival = 0;   // 最近一帧数据到达浏览器的时刻，用于端到端估算
let lastKfAt = 0;      // 关键帧请求节流
let requestedMode = null;  // 最近一次协商请求（供自测断言）
/* 单次连接的累计统计：断开时生成质量报告 */
const session = { t0: 0, n: 0, sum: 0, max: 0 };
const fpsWindow = [];

const hudVisible = () => !ui.hud.classList.contains('hidden');

let lastFrameW = 0;
let lastFrameH = 0;

/* 等比适配窗口。默认铺满窗口（旧行为，画面最大）；
   设置里可切到「原始像素（不放大）」：1 个流像素最多对应 1 个设备像素，
   低分辨率流不会被浏览器重采样放大，代价是四周留黑边。 */
function fitCanvas() {
  if (!lastFrameW || !lastFrameH) return;
  const fill = Math.min(window.innerWidth / lastFrameW, window.innerHeight / lastFrameH);
  const s = pref.fit === 'pixel'
    ? Math.min(fill, 1 / (window.devicePixelRatio || 1))
    : fill;
  ui.canvas.style.width = `${Math.round(lastFrameW * s)}px`;
  ui.canvas.style.height = `${Math.round(lastFrameH * s)}px`;
  /* 画布尺寸变了，输入层缓存的命中矩形要作废，否则本地光标会按旧尺寸判定 */
  input?.invalidateRect();
}
window.addEventListener('resize', fitCanvas);
// DPR 变化（拖到另一块屏/系统缩放改动）也要重新适配
try {
  matchMedia(`(resolution: ${window.devicePixelRatio}dppx)`)
    .addEventListener('change', () => fitCanvas());
} catch { /* 忽略旧浏览器 */ }

function onFrame(frame) {
  trace.decoded(performance.now(), frame.timestamp);
  if (rendered === 0) console.log('ww: first decoded frame', frame.displayWidth, frame.displayHeight);

  /* 分辨率变化后 SPS 变了，必须重建解码器，否则会静默停帧 */
  if (lastFrameW && frame.displayWidth !== lastFrameW) {
    console.log('ww: resolution changed', lastFrameW, '->', frame.displayWidth);
    decoder.configured = false;
    transport?.send({ t: 'kf' });
  }
  lastFrameW = frame.displayWidth;
  if (ui.canvas.width !== frame.displayWidth || ui.canvas.height !== frame.displayHeight) {
    ui.canvas.width = frame.displayWidth;
    ui.canvas.height = frame.displayHeight;
    lastFrameH = frame.displayHeight;
    fitCanvas();
    resolution = `${frame.displayWidth}×${frame.displayHeight}`;
  }
  const drawAt = performance.now();
  ctx.drawImage(frame, 0, 0, ui.canvas.width, ui.canvas.height);
  const drawMs = performance.now() - drawAt;
  renderStats.sum += drawMs;
  renderStats.n++;
  /* 上屏时刻 = 输入的"可见反馈"锚点，用于算 input→画面 的延迟 */
  trace.rendered(drawAt + drawMs, frame.timestamp);

  // 端到端估算：从数据到达浏览器到本帧上屏（服务端时钟不可跨机比较）
  if (lastArrival && rendered > 2) {
    e2e = performance.now() - lastArrival;
  }
  if (rendered === 0) {
    session.t0 = performance.now();
    session.n = 0;
    session.sum = 0;
    session.max = 0;
  } else if (session.t0) {
    session.n++;
    session.sum += e2e;
    if (e2e > session.max) session.max = e2e;
  }
  lastRenderedTs = frame.timestamp;
  frame.close();
  rendered++;
  {
    const t = performance.now();
    fpsWindow.push(t);
    /* 每次推入都裁剪，避免 HUD 隐藏时窗口无界增长、显示 100+ fps 的假数字 */
    while (fpsWindow.length && t - fpsWindow[0] > 1000) fpsWindow.shift();
  }
  if (rendered === 1) {
    setStep(1, 'done');
    setTimeout(() => setStep(2, 'done'), 120);
    ui.hud.classList.toggle('hidden', !pref.hud);
  }
}

let hudTimer = 0;
let audioAssertTick = 0;   // 每 10s 重申一次「要音频」，wwcap 中途重启后能自愈
function startHudLoop() {
  if (hudTimer) return;   // 重连不应重复叠加 interval
  hudTimer = setInterval(() => {
    if (!transport) return;

    // 无论 HUD 是否可见都要裁剪，否则数组无界增长
    const now = performance.now();
    while (fpsWindow.length && now - fpsWindow[0] > 1000) fpsWindow.shift();

    const bytesThisTick = transport.stats.bytes;
    transport.stats.bytes = 0;
    lastMbps = (bytesThisTick * 8) / 1e6;

    /* 实时质量警告：稳定几秒后再判断，避免重建编码器时的瞬时抖动误报 */
    const warnNow = pref.quality && !transport.closed && session.n > 30 && e2e > 120;
    ui.warn.classList.toggle('hidden', !warnNow);
    if (warnNow) {
      ui.warn.classList.toggle('bad', e2e > 200);
      ui.warn.textContent = `网络延迟偏高：${e2e.toFixed(0)} ms${e2e > 200 ? '，画面可能明显卡顿' : ''}`;
    }

    if (DEBUG) {
      dbgEl.textContent = `dg=${transport.stats.datagrams} kb=${(bytesThisTick / 1024).toFixed(0)} ` +
        `fr=${rendered} q=${decoder?.queue ?? 0} drop=${decoder?.dropped ?? 0}`;
    }
    if (++audioAssertTick >= 10) {
      audioAssertTick = 0;
      transport.send({ t: 'astream', rx: 1 });
    }

    /* 音频丢包反馈：网关据此调 RED 冗余档位（局域网基本恒为 0，弱网才开） */
    if (!transport.closed && audio) {
      const as = audio.stats;
      const total = as.packets + as.lost;
      const aloss = total > 0 ? as.lost / total : 0;
      transport.send({
        t: 'astats',
        aloss: Number(aloss.toFixed(4)),
        abuf: Math.round(as.bufferMs || 0),
      });
    }
    /* 自动上传状态：让"有没有在传"随时可见，不用等手动导出 */
    if (ui.setTraceOut) {
      const up = traceUp.stat();
      const lagStat = trace.stats().inputLag;
      ui.setTraceOut.textContent = up.sent
        ? `自动上传 ${up.sent} 份${up.failures ? ` · 失败 ${up.failures}` : ''}${lagStat ? ` · p95 ${lagStat.p95}ms` : ''}`
        : (up.lastError ? `上传失败：${up.lastError}` : '等待首次上传…');
    }

    if (!hudVisible()) return;

    ui.fps.textContent = `${fpsWindow.length} fps`;
    ui.bitrate.textContent = `${(bytesThisTick * 8 / 1e6).toFixed(1)} Mbps`;
    ui.bitrateTarget.textContent = targetBitrateLabel();
    ui.res.textContent = resolution;
    ui.latency.textContent = `${e2e.toFixed(0)} ms`;

    const q = decoder?.queue ?? 0;
    const s = Number(pref.strategy);
    const strategyText = s === 2 ? '画质优先（受硬件限制）'
      : s === 1 ? '流畅优先' : s === 0 ? '平衡' : `自定义 QP ${pref.qp}`;
    ui.tier.textContent = q > 3 ? `队列 ${q}（降档）` : strategyText;
    ui.latency.className = e2e > 120 ? 'bad' : e2e > 70 ? 'warn' : '';

    transport.send({ t: 'stats', q, loss: transport.stats.lost, fps: fpsWindow.length });
  }, 1000);
}

/* 断开后把本次连接的质量摊开：延迟、丢帧、时长，给出一个直观结论 */
function showReport({ frames, avg, max, dropped, skipped, seconds }) {
  const dur = seconds >= 60
    ? `${Math.floor(seconds / 60)} 分 ${Math.round(seconds % 60)} 秒`
    : `${seconds.toFixed(0)} 秒`;
  const bad = avg >= 120;
  const verdict = avg < 60 && (dropped + skipped) < frames * 0.02 ? '良好' : bad ? '较差' : '一般';
  const rows = [
    ['时长', dur],
    ['平均延迟', `${avg.toFixed(0)} ms`],
    ['最大延迟', `${max.toFixed(0)} ms`],
    ['渲染帧数', String(frames)],
    ['丢帧 / 跳过', `${dropped} / ${skipped}`],
    ['结论', verdict],
  ];
  ui.reportList.innerHTML = '';
  for (const [k, v] of rows) {
    const row = document.createElement('div');
    const dt = document.createElement('dt');
    const dd = document.createElement('dd');
    dt.textContent = k;
    dd.textContent = v;
    row.append(dt, dd);
    ui.reportList.appendChild(row);
  }
  ui.report.classList.remove('hidden');
}

/* 断线/出错后的完整回收：以前只弹一句“刷新页面重连”，但连接按钮被隐藏、
   transport 也不置空，用户只能 F5。现在直接回到可重连状态。 */
let resetting = false;
function resetSession(msg) {
  if (resetting) return;
  resetting = true;
  /* 断开前把最后一段追踪送出去（fetch 用 keepalive，页面不关也能落地） */
  traceUp.stop();
  enableLocalCursor(false);   // 断开后交还给系统光标
  const dropped = decoder?.dropped ?? 0;
  const skipped = decoder?.skipped ?? 0;
  if (pref.quality && rendered > 0 && session.t0) {
    showReport({
      frames: rendered,
      avg: session.n ? session.sum / session.n : 0,
      max: session.max,
      dropped,
      skipped,
      seconds: (performance.now() - session.t0) / 1000,
    });
  }
  session.t0 = 0;
  session.n = 0;
  session.sum = 0;
  session.max = 0;
  ui.warn.classList.add('hidden');
  ui.help.classList.add('hidden');
  rtts.length = 0;
  chromaName = '—';
  activeCodec = null;
  renderStats.sum = 0;
  renderStats.n = 0;
  rate.t = 0;
  restartSeq++;   // 取消还在等待重启结果的轮询
  try { ui.btnRestart.disabled = false; ui.btnRestartDeep.disabled = false; } catch { /* ignore */ }
  try { input?.destroy(); } catch { /* ignore */ }
  input = null;
  /* 音频随会话回收：AudioContext 一起关掉，重连时在用户手势里重新创建 */
  try { audio?.close(); } catch { /* ignore */ }
  audio = null;
  const t = transport;
  transport = null;
  try { t?.close?.(); } catch { /* ignore */ }
  try { decoder?.decoder?.close(); } catch { /* ignore */ }
  decoder = null;
  rendered = 0;
  lastFrameW = 0;
  lastFrameH = 0;
  e2e = 0;
  lastArrival = 0;
  resolution = '—';
  codecName = '—';
  fpsWindow.length = 0;
  ui.hud.classList.add('hidden');
  ui.stage.classList.add('hidden');
  ui.connect.classList.remove('hidden');
  ui.enter.disabled = false;
  setStep(0, 'active');
  if (msg) showErrors([msg]);
  resetting = false;
}

/* ── 音频（下行）───────────────────────────────────────────────
   整机混音 → Opus → AudioWorklet。AudioContext 必须在用户手势里同步创建，
   否则自动播放策略会把 ctx 挂起（表现为「连上了但没有声音」）。 */
function audioStateText() {
  if (!audio) return '未连接';
  if (audio.error) return `初始化失败：${audio.error}`;
  if (!audio.ready) return '初始化中…';
  const s = audio.stats;
  return `${audio.ctx?.state ?? '-'} · 缓冲 ${Math.round(s.bufferMs || 0)}/${audio.targetMs} ms` +
    ` · 补帧 ${s.recovered} · 丢帧 ${s.lost} · 欠载 ${s.underrun}`;
}

function startAudio() {
  if (!audio) audio = new AudioPlayer();
  audio.setVolume(pref.vol / 100);
  audio.setMuted(false);
  return audio.init().catch((e) => {
    console.warn('ww: 音频初始化失败', e);
    audio.error = e?.message || String(e);
    return null;
  });
}

async function enter(fullscreen) {
  if (transport && !transport.closed) return;
  ui.enter.disabled = true;
  ui.report.classList.add('hidden');
  ui.warn.classList.add('hidden');
  setStep(0, 'active');
  showErrors([]);

  try {
    /* 同步创建 AudioContext（保住用户手势），后续初始化并行进行 */
    const audioReady = startAudio();

    // 取 WebTransport 证书哈希做钉扎（自签证书的唯一免信任库路径）
    const wtOpts = {};
    try {
      const r = await fetch('/cert-hash.json', { cache: 'no-store' });
      if (r.ok) {
        const { hash } = await r.json();
        wtOpts.serverCertificateHashes = [{
          algorithm: 'sha-256',
          value: Uint8Array.from(atob(hash), (c) => c.charCodeAt(0)),
        }];
      }
    } catch { /* 无钉扎信息则退回系统信任库 */ }

    transport = new Transport(`https://${host}:${port}/stream`, {
      onChunk: (c) => {
        lastArrival = performance.now();
        trace.frame({ id: c.frameId, pts: c.ptsUs, t: c.arrivedAt, key: c.key, bytes: c.data.length });
        const useCodec = (codec) => {
          codecName = codecLabel(codec);
          /* profile 244 = H.264 High 4:4:4 Predictive，其余按 4:2:0 显示 */
          chromaName = parseInt(codec.slice(5, 7), 16) === 0xf4 ? '4:4:4' : '4:2:0';
          activeCodec = codec;
          decoder.configure({ fps: pref.fps || 60, codec });
          console.log('ww: decoder configured', codec);
        };
        const parsed = codecFromAnnexB(c.data, true);   // 仅当分片真的带 SPS 时非 null
        if (!decoder.configured) {
          // 重建解码器时必须从关键帧开始；请求要节流，避免每帧都发
          if (!c.key) {
            const now = performance.now();
            if (now - lastKfAt > 300) { lastKfAt = now; transport?.send({ t: 'kf' }); }
            return;
          }
          useCodec(parsed || activeCodec || 'avc1.640033');
        } else if (parsed && parsed !== activeCodec) {
          /* 4:4:4 开关会改变 SPS profile，必须重建解码器；等带 SPS 的关键帧 */
          if (!c.key) {
            const now = performance.now();
            if (now - lastKfAt > 300) { lastKfAt = now; transport?.send({ t: 'kf' }); }
            return;
          }
          useCodec(parsed);
        }
        decoder.push(c);
      },
      onAudio: (buf) => audio?.push(buf),
      onCtrl,
      /* 断线原因只进 UI 的话，排障时看不到（自动化测试也抓不到），这里同时打到控制台 */
      onClose: () => { console.warn('ww: WebTransport 已关闭'); resetSession('连接已断开，可直接重新连接'); },
      onError: (e) => {
        console.error('ww: 传输错误', e?.name, e?.message ?? e);
        resetSession(`传输错误：${e?.message ?? e}`);
      },
    }, wtOpts);

    decoder = new Decoder({
      hw: pref.decode,
      onFrame,
      onError: (e) => {
        console.error('decode', e?.name, e?.message);
        transport?.send({ t: 'kf' });   // 出错后立刻要一个 IDR 重建解码链
      },
      onNeedKeyframe: () => {
        const now = performance.now();
        if (now - lastKfAt > 300) { lastKfAt = now; transport?.send({ t: 'kf' }); }
      },
    });

    console.log('ww: wt created, waiting ready');
    await transport.connect();
    console.log('ww: connected');
    setStep(0, 'done');
    setStep(1, 'active');

    transport.send({ t: 'hello', w: window.innerWidth, h: window.innerHeight, dpr: devicePixelRatio, fps: 60 });
    /* 声明要下行音频：网关在会话建立时也会发一次，这里保证「客户端主动重连」同样生效 */
    transport.send({ t: 'astream', rx: 1 });
    audioReady.then(() => {
      if (pref.muteHost) transport?.send({ t: 'mute', on: 1 });
    });

    /* 自动协商：物理像素做输出，scale 用 DPR（2x UI），逻辑尺寸与浏览器 CSS 尺寸一致 */
    setTimeout(() => {
      if (pref.res !== 'auto') {
        /* 集显上 3200/4K 实测只有 20~30fps、抓帧 55ms、客户端解码上百毫秒，
           连上去就是"鼠标几乎用不了"。连接时临时降到 1080p，
           但**不改**用户保存的偏好（切到独显后仍然按原值生效）。 */
        const highRes = pref.res === '3200x1800' || pref.res === '3840x2160';
        if (highRes && caps.encoder && caps.encoder !== 'nvenc') {
          ui.setRes.value = '1920x1080';
          if (ui.setResNote) {
            ui.setResNote.textContent =
              `集显下 ${pref.res} 只有 20~30fps，本次会话已临时降到 1920×1080（保存的设置未改）。`;
            ui.setResNote.classList.remove('hidden');
          }
          console.warn('ww: 集显下', pref.res, '过慢，本次会话改用 1920x1080');
          requestRes('1920x1080');
        } else {
          requestRes(pref.res);
        }
      }
      else autoMode();
      /* 始终下发帧率：pref.fps=0（自动）要显式发 0 把服务端恢复到默认值，
         否则上一个会话设的 30fps 会一直留在 wwcap 里，鼠标看起来"不跟手" */
      requestFps(pref.fps);
      /* 只在用户手动选过编码器时下发；空串表示跟随服务端启动参数。
         以前每次连接都无条件下发，会把集显机器切到 nvenc 导致编码器创建失败。 */
      if (pref.enc) transport?.send({ t: 'enc', v: pref.enc });
      /* 策略与 4:4:4 独立即时生效；只有「自定义」才单独下发 QP */
      if (Number(pref.strategy) === 3) {
        requestQp(pref.qp);
      } else {
        transport?.send({ t: 'strategy', v: Number(pref.strategy) || 0 });
      }
      transport?.send({ t: 'chroma', v: pref.chroma ? 1 : 0 });
      /* 目标码率放在最后：策略/AI 档位会改 QP，码率设置要覆盖它 */
      if (pref.bitrate > 0) requestBitrate(pref.bitrate);
      if (pref.enc) setTimeout(refreshCaps, 700);
    }, 500);
    let modeTimer = 0;
    window.addEventListener('resize', () => {
      clearTimeout(modeTimer);
      modeTimer = setTimeout(() => { if (!manualRes) autoMode(); }, 300);
    });

    ui.connect.classList.add('hidden');
    ui.stage.classList.remove('hidden');
    ui.stats.classList.toggle('hidden', !pref.stats);
    if (pref.stats) rate.t = 0;

    /* 每条出站输入先过 trace：补客户端时间戳 + 记账（不发则丢弃） */
    input = new Input(ui.canvas, (m) => { if (transport && !transport.closed) transport.send(trace.input(m)); }, {
      /* Ctrl+Alt+F1 切换串流状态叠层（HUD），与叠层上的提示保持一致 */
      onToggleHud: () => setHudVisible(!pref.hud),
      /* 本地光标跟随真实指针，每帧同步更新（不经 rAF 节流） */
      onPointer: (cx, cy) => placeLocalCursor(cx, cy),
    });

    if (fullscreen || pref.mode === 'fullscreen') {
      // 无用户手势时全屏会被拒绝，不能让它中断串流启动
      try { await document.documentElement.requestFullscreen?.(); } catch { /* ignore */ }
      /* 桌面场景默认不做指针锁定：锁定后只能用相对位移，和远端光标容易错位。
         需要"游戏式"相对鼠标时用 ?lock=1 打开。 */
      if (new URLSearchParams(location.search).has('lock')) {
        try { ui.canvas.requestPointerLock?.(); } catch { /* ignore */ }
      }
    }
    startHudLoop();
    traceUp.start();          // 连上就开始异步上传追踪窗口
  } catch (e) {
    /* 连接失败必须回收半成品 transport，否则下次点击被 `if (transport) return` 挡住，
       且 status().connected 会把未连上的对象误报成已连接 */
    try { transport?.close?.(); } catch { /* ignore */ }
    try { audio?.close(); } catch { /* ignore */ }
    transport = null;
    decoder = null;
    audio = null;
    ui.enter.disabled = false;
    setStep(0, 'active');
    showErrors([
      `无法建立 WebTransport：${e?.message ?? e}`,
      '确认服务端 wwgw 已启动、证书已信任、来源在 192.168.0.0/24 内',
    ]);
  }
}

ui.enter.addEventListener('click', () => enter(false));
ui.enterFs.addEventListener('click', () => enter(true));

// 直连：打开 ?auto=1 自动进入并全屏（便于做成桌面快捷方式）
if (new URLSearchParams(location.search).has('auto')) {
  enter(true);
}

// 内置自测接口：Playwright / CDP 可直接断言，无需读控制台
window.__ww = {
  status: () => ({
    connected: !!transport?.connected && !transport.closed,
    datagrams: transport?.stats.datagrams ?? 0,
    lost: transport?.stats.lost ?? 0,
    rendered,
    resolution,
    e2eMs: Number(e2e.toFixed(1)),
    decoderQueue: decoder?.queue ?? 0,
    dropped: decoder?.dropped ?? 0,
    skipped: decoder?.skipped ?? 0,
    fps: fpsWindow.length,
    requested: requestedMode,
    frames: transport?.stats.frames ?? 0,
    incomplete: transport?.stats.incomplete ?? 0,
    late: transport?.stats.late ?? 0,
    rttMs: rtts.length ? Number((rtts.reduce((a, b) => a + b, 0) / rtts.length).toFixed(1)) : null,
    decodeMs: decoder?.decodeMs ?? null,
    queueMs: decoder?.queueMs ?? null,
    /* 音频：audioPackets 来自传输层（不依赖 AudioContext 是否可用），
       其余来自播放器，便于 headless 环境分别断言 */
    audioPackets: transport?.stats.audio.packets ?? 0,
    audioDecoded: audio?.stats.decoded ?? 0,
    audioLost: audio?.stats.lost ?? 0,
    audioRecovered: audio?.stats.recovered ?? 0,
    audioUnderrun: audio?.stats.underrun ?? 0,
    audioBufferMs: audio ? Number((audio.stats.bufferMs || 0).toFixed(1)) : null,
    audioTargetMs: audio?.targetMs ?? null,
    audioState: audio?.ctx?.state ?? 'none',
    audioError: audio?.error ?? null,
    statsVisible: !ui.stats.classList.contains('hidden'),
    strategy: Number(pref.strategy),
    chroma444: !!pref.chroma,          // 用户偏好
    chroma: chromaName,                // 实际生效（从 SPS 推断）
    codec: activeCodec,
    decode444,
  }),
  /* 注入类消息也走 trace，保证自动化压测与真人操作产出同构的追踪数据 */
  pointer: (x, y) => transport?.send(trace.input({ t: 'pm', x, y })),
  key: (k, down) => transport?.send(trace.input({ t: 'kk', k, d: down ? 1 : 0 })),
  requestKeyframe: () => transport?.send({ t: 'kf' }),
  trace: () => trace.dump({ resolution, strategy: pref.strategy, fps: pref.fps,
                            enc: pref.enc, chroma: pref.chroma, status: window.__ww.status() }),
  traceStats: () => trace.stats(),
  /* 自动上传：最近一次统计 / 立刻推一份（自动化取数用） */
  uploadStat: () => traceUp.stat(),
  uploadNow: () => traceUp.flush('manual'),
  /* 让 wwcap 立刻把环形缓冲落盘（导出/自动化取数前调用，避免等 5 秒周期） */
  traceFlush: () => transport?.send({ t: 'trace', v: 1 }),
  resetTrace: () => { trace.reset(); return trace.stats(); },
  exportTrace: () => exportTrace(),
};

if (!('WebTransport' in window)) {
  showErrors(['当前浏览器不支持 WebTransport，请使用 Chrome/Edge 最新版']);
}
if (!('VideoDecoder' in window)) {
  showErrors(['当前浏览器不支持 WebCodecs，请使用 Chrome/Edge 最新版']);
}
/* 音频缺 AudioDecoder 时只降级为无声，不影响画面 */
if (!('AudioDecoder' in window)) {
  console.warn('ww: 浏览器不支持 AudioDecoder，本次串流将没有声音');
}

/* ── 设置面板事件绑定 ────────────────────────────────────────── */
ui.setRes.value = pref.res;
ui.setFit.value = pref.fit;
ui.setFps.value = String(pref.fps);
ui.setHud.checked = !!pref.hud;
ui.setEnc.value = pref.enc;
ui.setQp.value = String(qpToSlider(pref.qp));
ui.setMode.value = pref.mode;
ui.setDecode.value = pref.decode;
ui.setQuality.checked = !!pref.quality;
ui.setStats.checked = !!pref.stats;
ui.setVol.value = String(pref.vol);
ui.setMuteHost.checked = !!pref.muteHost;
ui.setMuteBlur.checked = !!pref.muteBlur;
renderQp();
renderStrategy();
renderBitrate();
renderResNote();
renderVol();
probeDecode444().then(renderChroma);
refreshCaps();

ui.setEnc.addEventListener('change', () => {
  pref.enc = ui.setEnc.value;
  savePref();
  transport?.send({ t: 'enc', v: pref.enc });
  renderChroma();
  /* 服务端切换编码器后会重写能力文件，稍后再拉一次 */
  setTimeout(refreshCaps, 700);
  console.log('ww: 编码器 ->', pref.enc);
});

/* 三种策略：只改 QP 与目标帧率，不碰分辨率 */
ui.setStrategy.addEventListener('click', (e) => {
  const btn = e.target.closest('button[data-strategy]');
  if (!btn) return;
  const s = Number(btn.dataset.strategy);
  pref.strategy = s;
  pref.qp = STRATEGY_QP[s] ?? pref.qp;
  ui.setQp.value = String(qpToSlider(pref.qp));
  savePref();
  renderStrategy();
  renderQp();
  transport?.send({ t: 'strategy', v: s });
  if (s === 1) requestFps(pref.fps);   // 流畅优先固定 60/30
  console.log('ww: 策略 ->', STRATEGY_LABEL[s]);
});

/* 目标码率：拖动只更新显示，松手才下发（避免刷爆控制流）。
   0 = 关闭，服务端重建回 CQP 并按当前 QP 编码。 */
ui.setBitrate.addEventListener('input', () => {
  pref.bitrate = Number(ui.setBitrate.value) || 0;
  renderBitrate();
});
ui.setBitrate.addEventListener('change', () => {
  savePref();
  requestBitrate(pref.bitrate);
  renderBitrate();
  console.log('ww: 目标码率 ->', pref.bitrate > 0 ? `${pref.bitrate} Mbps` : '关闭（QP）');
});

ui.setChroma.addEventListener('change', () => {
  pref.chroma = ui.setChroma.checked;
  savePref();
  renderChroma();
  transport?.send({ t: 'chroma', v: pref.chroma ? 1 : 0 });
  console.log('ww: 4:4:4 ->', pref.chroma ? '开' : '关');
});

ui.gear.addEventListener('click', () => ui.settings.classList.toggle('hidden'));
ui.setClose.addEventListener('click', () => ui.settings.classList.add('hidden'));
ui.reportClose.addEventListener('click', () => ui.report.classList.add('hidden'));

ui.setRes.addEventListener('change', () => {
  pref.res = ui.setRes.value;
  savePref();
  requestRes(pref.res);
  renderResNote();
});
ui.setFit.addEventListener('change', () => {
  pref.fit = ui.setFit.value;
  savePref();
  fitCanvas();
});
ui.setFps.addEventListener('change', () => {
  pref.fps = Number(ui.setFps.value);
  savePref();
  requestFps(pref.fps);
});
ui.setHud.addEventListener('change', () => setHudVisible(ui.setHud.checked));

/* 画质滑块：拖动时只更新本地显示，松手才下发 QP，避免刷爆控制流；
   手动拖 QP 即进入「自定义」，策略不再覆盖 */
ui.setQp.addEventListener('input', () => {
  pref.qp = sliderToQp(ui.setQp.value);
  if (Number(pref.strategy) !== 3) {
    pref.strategy = 3;
    renderStrategy();
  }
  renderQp();
});
ui.setQp.addEventListener('change', () => {
  savePref();
  requestQp(pref.qp);
  renderQp();
});

ui.setMode.addEventListener('change', () => {
  pref.mode = ui.setMode.value;
  savePref();
  applyMode(pref.mode);
});

/* 切换解码偏好要重建解码器，否则新配置不会生效 */
ui.setDecode.addEventListener('change', () => {
  pref.decode = ui.setDecode.value;
  savePref();
  if (decoder) {
    decoder.hw = pref.decode;
    decoder.configured = false;
    transport?.send({ t: 'kf' });
  }
});

ui.setQuality.addEventListener('change', () => {
  pref.quality = ui.setQuality.checked;
  savePref();
  if (!pref.quality) ui.warn.classList.add('hidden');
});

ui.setStats.addEventListener('change', () => setStatsVisible(ui.setStats.checked));

/* ── 音频设置：本地音量只动 GainNode，目标机静音才走 ctrl ───────── */
function requestMuteHost(on) {
  pref.muteHost = !!on;
  savePref();
  transport?.send({ t: 'mute', on: pref.muteHost ? 1 : 0 });
}

ui.setVol.addEventListener('input', () => {
  pref.vol = Number(ui.setVol.value);
  renderVol();
  audio?.setVolume(pref.vol / 100);   // 拖动过程只改本地增益，松手再落盘
});
ui.setVol.addEventListener('change', () => savePref());
ui.setMuteHost.addEventListener('change', () => requestMuteHost(ui.setMuteHost.checked));
ui.setMuteBlur.addEventListener('change', () => {
  pref.muteBlur = ui.setMuteBlur.checked;
  savePref();
  if (!pref.muteBlur) audio?.setMuted(false);
});
window.addEventListener('blur', () => { if (pref.muteBlur) audio?.setMuted(true); });
window.addEventListener('focus', () => { if (pref.muteBlur) audio?.setMuted(false); });

/* 自动播放策略：?auto=1（无手势）进入时 AudioContext 会停在 suspended，
   首次点击/按键再恢复，避免用户「连上了但没有声音」却找不到原因 */
const kickAudio = () => {
  if (audio?.ctx?.state === 'suspended') audio.ctx.resume().catch(() => {});
};
ui.canvas.addEventListener('pointerdown', kickAudio);
window.addEventListener('keydown', kickAudio);

/* ── 串流控制：浏览器侧主动重启服务 ─────────────────────────────
   判定「恢复成功」要两个证据：
   1) 服务端的 restarts 计数真的 +1（/healthz，同源，不受跨域限制）；
   2) 之后帧计数继续增长。
   只看帧计数会在「旧链路本来就在出帧」时误报已恢复。 */
let restartSeq = 0;

function setRestartNote(text, kind = '') {
  if (!ui.setRestartNote) return;
  ui.setRestartNote.textContent = text;
  ui.setRestartNote.className = kind;
}

async function fetchRestarts() {
  try {
    const r = await fetch('/healthz', { cache: 'no-store' });
    if (!r.ok) return null;
    return Number((await r.json()).restarts) || 0;
  } catch {
    return null;
  }
}

function requestRestart(deep) {
  if (!transport || transport.closed) {
    setRestartNote('未连接，无法下发重启', 'bad');
    return;
  }
  if (deep && !window.confirm('强制恢复会重启整个桌面会话，桌面里打开的程序都会被关闭。继续？')) {
    return;
  }

  const seq = ++restartSeq;
  setRestartNote(deep ? '正在重启桌面会话（约 10–15s）…' : '正在重启采集…', 'warn');
  ui.btnRestart.disabled = true;
  ui.btnRestartDeep.disabled = true;

  (async () => {
    /* 一定要先取 restarts 基线再下发，否则服务端可能已经加过 1，永远等不到「变大」 */
    const epoch0 = await fetchRestarts();
    if (seq !== restartSeq) return;
    transport.send({ t: 'restart', deep: deep ? 1 : 0 });

    const beforeFrames = window.__ww?.status?.().frames ?? 0;
    const t0 = performance.now();
    const limit = deep ? 35 : 12;

    for (;;) {
      await new Promise((r) => setTimeout(r, 400));
      if (seq !== restartSeq) return;   // 会话已回收，放弃后续提示

      const elapsed = (performance.now() - t0) / 1000;
      const s = window.__ww?.status?.();

      if (!s || !s.connected) {
        setRestartNote(deep ? '会话已重建，请刷新页面重连' : '连接已断开，请刷新页面', 'warn');
        break;
      }
      const epoch = await fetchRestarts();
      const accepted = epoch0 == null ? elapsed > 2 : (epoch != null && epoch > epoch0);
      if (accepted && s.frames > beforeFrames + 5) {
        /* 旧 wwcap 退出时会恢复扬声器静音状态，新进程不知道这条偏好，这里补发一次 */
        if (pref.muteHost) transport?.send({ t: 'mute', on: 1 });
        setRestartNote(`已恢复（${elapsed.toFixed(1)}s）`, 'ok');
        break;
      }
      if (elapsed > limit) {
        setRestartNote(
          deep ? '仍未出图，看 /tmp/ww-restart.log' : '仍未出图，可再试「强制恢复桌面」',
          'bad',
        );
        break;
      }
    }
    if (seq !== restartSeq) return;
    ui.btnRestart.disabled = false;
    ui.btnRestartDeep.disabled = false;
  })();
}

/* 串流控制按钮：重启采集 / 强制恢复桌面 */
ui.btnRestart.addEventListener('click', () => requestRestart(false));
ui.btnRestartDeep.addEventListener('click', () => requestRestart(true));

/* 输入追踪：导出（客户端+服务端合并）/ 清空重录 */
ui.btnTrace?.addEventListener('click', () => { exportTrace().catch((e) => console.error('trace', e)); });
ui.btnTraceReset?.addEventListener('click', () => {
  trace.reset();
  if (ui.setTraceOut) ui.setTraceOut.textContent = '已清空，开始记录';
});

ui.setHelp.addEventListener('click', () => setHelpVisible(true));
ui.helpClose.addEventListener('click', () => setHelpVisible(false));
ui.help.addEventListener('click', (e) => {
  if (e.target === ui.help) setHelpVisible(false);
});
document.addEventListener('keydown', (e) => {
  if (e.key === 'Escape' && !ui.help.classList.contains('hidden')) setHelpVisible(false);
});

/* 统计信息每秒刷新一次（隐藏时不产生任何流量） */
setInterval(updateStats, 1000);

/* 全屏键（Esc）或系统全屏变化时，让下拉框反映真实状态 */
document.addEventListener('fullscreenchange', () => {
  ui.setMode.value = document.fullscreenElement ? 'fullscreen' : 'window';
});

/* 面板里显示当前真实状态（分辨率/帧率/端到端/丢帧） */
setInterval(() => {
  if (ui.settings.classList.contains('hidden')) return;
  renderQp();
  renderVol();
  ui.setAudioState.textContent = audioStateText();
  const s = window.__ww?.status?.();
  if (!s) { ui.setNote.textContent = '未连接'; return; }
  ui.setNote.textContent = `${s.resolution} · ${s.fps} fps · ${s.e2eMs} ms` +
    (s.skipped ? ` · 丢帧 ${s.skipped}` : '');
}, 800);
