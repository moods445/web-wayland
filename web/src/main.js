// web-wayland 前端主控
import { Transport } from './transport.js';
import { Decoder, codecFromAnnexB } from './decoder.js';
import { Input } from './input.js';

const $ = (id) => document.getElementById(id);

const ui = {
  connect: $('connect'), stage: $('stage'), canvas: $('screen'),
  cursor: $('cursor'), hud: $('hud'),
  enter: $('enter'), enterFs: $('enter-fs'), steps: $('steps'), errors: $('errors'),
  addr: $('server-addr'), copy: $('copy-addr'),
  bitrate: $('hud-bitrate'), fps: $('hud-fps'), res: $('hud-res'),
  latency: $('hud-latency'), tier: $('hud-tier'),
  gear: $('gear'), settings: $('settings'), setRes: $('set-res'), setFps: $('set-fps'), setEnc: $('set-enc'),
  setHud: $('set-hud'), setFs: $('set-fs'), setClose: $('set-close'), setNote: $('set-note'),
};

/* ── 客户端串流设置（只影响本机这条连接）──────────────────────
   分辨率/帧率是「请求」：服务端按档位与上限执行，并在自动模式下按实测帧率升降档。
   手动选分辨率会带 lock=1，关闭服务端的自动升降档，避免和用户选择打架。 */
const pref = Object.assign({ res: 'auto', fps: 0, hud: true, enc: 'vaapi' },
  JSON.parse(localStorage.getItem('ww.pref') || '{}'));
const savePref = () => localStorage.setItem('ww.pref', JSON.stringify(pref));
let manualRes = null;   // 非 auto 时禁止自动跟随窗口

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
  const tiers = [1920, 2560, 3200, 3840];
  let w = tiers[0];
  for (const t of tiers) if (Math.abs(t - reqW) < Math.abs(w - reqW)) w = t;
  const h = (w * 9 / 16) & ~1;
  requestedMode = { w, h, scale: dpr };
  transport?.send({ t: 'mode', w: reqW, h: reqH, scale: dpr, lock: 0 });
  console.log('ww: auto mode', reqW, reqH, 'scale', dpr, '→ 档位', w, h);
}

function requestFps(v) {
  transport?.send({ t: 'fps', v: Number(v) || 0 });
}

function applyFullscreen() {
  if (document.fullscreenElement) document.exitFullscreen?.();
  else document.documentElement.requestFullscreen?.().catch(() => {});
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

const ctx = ui.canvas.getContext('2d', { alpha: false, desynchronized: true });
let transport = null, decoder = null, input = null;
let rendered = 0, lastHudAt = 0, lastRenderedTs = 0, e2e = 0, resolution = '—', chunkLogged = false;
let lastArrival = 0;   // 最近一帧数据到达浏览器的时刻，用于端到端估算
let lastKfAt = 0;      // 关键帧请求节流
let requestedMode = null;  // 最近一次协商请求（供自测断言）
const fpsWindow = [];

const hudVisible = () => !ui.hud.classList.contains('hidden');

let lastFrameW = 0;
let lastFrameH = 0;

/* 等比适配窗口；当窗口设备像素≈流尺寸时就是 1:1，不产生额外重采样 */
function fitCanvas() {
  if (!lastFrameW || !lastFrameH) return;
  const s = Math.min(window.innerWidth / lastFrameW, window.innerHeight / lastFrameH);
  ui.canvas.style.width = `${Math.round(lastFrameW * s)}px`;
  ui.canvas.style.height = `${Math.round(lastFrameH * s)}px`;
}
window.addEventListener('resize', fitCanvas);

function onFrame(frame) {
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
  ctx.drawImage(frame, 0, 0, ui.canvas.width, ui.canvas.height);

  // 端到端估算：从数据到达浏览器到本帧上屏（服务端时钟不可跨机比较）
  if (lastArrival && rendered > 2) {
    e2e = performance.now() - lastArrival;
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

function startHudLoop() {
  setInterval(() => {
    if (!transport) return;

    // 无论 HUD 是否可见都要裁剪，否则数组无界增长
    const now = performance.now();
    while (fpsWindow.length && now - fpsWindow[0] > 1000) fpsWindow.shift();

    const bytesThisTick = transport.stats.bytes;
    transport.stats.bytes = 0;

    if (DEBUG) {
      dbgEl.textContent = `dg=${transport.stats.datagrams} kb=${(bytesThisTick / 1024).toFixed(0)} ` +
        `fr=${rendered} q=${decoder?.queue ?? 0} drop=${decoder?.dropped ?? 0}`;
    }
    if (!hudVisible()) return;

    ui.fps.textContent = `${fpsWindow.length} fps`;
    ui.bitrate.textContent = `${(bytesThisTick * 8 / 1e6).toFixed(1)} Mbps`;
    ui.res.textContent = resolution;
    ui.latency.textContent = `${e2e.toFixed(0)} ms`;

    const q = decoder?.queue ?? 0;
    ui.tier.textContent = q > 3 ? `队列 ${q}（降档）` : `QP 自适应`;
    ui.latency.className = e2e > 120 ? 'bad' : e2e > 70 ? 'warn' : '';

    transport.send({ t: 'stats', q, loss: transport.stats.lost, fps: fpsWindow.length });
  }, 1000);
}

async function enter(fullscreen) {
  if (transport) return;
  ui.enter.disabled = true;
  setStep(0, 'active');
  showErrors([]);

  try {
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
        if (!decoder.configured) {
          // 重建解码器时必须从关键帧开始；请求要节流，避免每帧都发
          if (!c.key) {
            const now = performance.now();
            if (now - lastKfAt > 300) { lastKfAt = now; transport?.send({ t: 'kf' }); }
            return;
          }
          const codec = codecFromAnnexB(c.data);
          decoder.configure({ fps: 60, codec });
          console.log('ww: decoder configured', codec);
        }
        decoder.push(c);
      },
      onClose: () => showErrors(['连接已断开，刷新页面重连']),
      onError: (e) => showErrors([`传输错误：${e?.message ?? e}`]),
    }, wtOpts);

    decoder = new Decoder({
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

    /* 自动协商：物理像素做输出，scale 用 DPR（2x UI），逻辑尺寸与浏览器 CSS 尺寸一致 */
    setTimeout(() => {
      if (pref.res !== 'auto') requestRes(pref.res);
      else autoMode();
      if (pref.fps) requestFps(pref.fps);
      /* 始终下发编码器，保证面板显示与实际一致（服务端可能以 --enc nvenc 启动） */
      transport?.send({ t: 'enc', v: pref.enc });
    }, 500);
    let modeTimer = 0;
    window.addEventListener('resize', () => {
      clearTimeout(modeTimer);
      modeTimer = setTimeout(() => { if (!manualRes) autoMode(); }, 300);
    });

    ui.connect.classList.add('hidden');
    ui.stage.classList.remove('hidden');

    input = new Input(ui.canvas, ui.cursor, (m) => transport.send(m), {
      onToggleHud: () => {
        const visible = ui.hud.classList.toggle('hidden') === false;
        pref.hud = visible;
        ui.setHud.checked = visible;
        savePref();
      },
    });

    if (fullscreen) {
      // 无用户手势时全屏会被拒绝，不能让它中断串流启动
      try { await document.documentElement.requestFullscreen?.(); } catch { /* ignore */ }
      /* 桌面场景默认不做指针锁定：锁定后只能用相对位移，和远端光标容易错位。
         需要"游戏式"相对鼠标时用 ?lock=1 打开。 */
      if (new URLSearchParams(location.search).has('lock')) {
        try { ui.canvas.requestPointerLock?.(); } catch { /* ignore */ }
      }
    }
    startHudLoop();
  } catch (e) {
    /* 连接失败必须回收半成品 transport，否则下次点击被 `if (transport) return` 挡住，
       且 status().connected 会把未连上的对象误报成已连接 */
    try { transport?.close?.(); } catch { /* ignore */ }
    transport = null;
    decoder = null;
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
  }),
  pointer: (x, y) => transport?.send({ t: 'pm', x, y }),
  key: (k, down) => transport?.send({ t: 'kk', k, d: down ? 1 : 0 }),
  requestKeyframe: () => transport?.send({ t: 'kf' }),
};

if (!('WebTransport' in window)) {
  showErrors(['当前浏览器不支持 WebTransport，请使用 Chrome/Edge 最新版']);
}
if (!('VideoDecoder' in window)) {
  showErrors(['当前浏览器不支持 WebCodecs，请使用 Chrome/Edge 最新版']);
}

/* ── 设置面板事件绑定 ────────────────────────────────────────── */
ui.setRes.value = pref.res;
ui.setFps.value = String(pref.fps);
ui.setHud.checked = !!pref.hud;
ui.setEnc.value = pref.enc;
ui.setEnc.addEventListener('change', () => {
  pref.enc = ui.setEnc.value;
  savePref();
  transport?.send({ t: 'enc', v: pref.enc });
  console.log('ww: 编码器 ->', pref.enc);
});

ui.gear.addEventListener('click', () => ui.settings.classList.toggle('hidden'));
ui.setClose.addEventListener('click', () => ui.settings.classList.add('hidden'));
ui.setFs.addEventListener('click', applyFullscreen);

ui.setRes.addEventListener('change', () => {
  pref.res = ui.setRes.value;
  savePref();
  requestRes(pref.res);
});
ui.setFps.addEventListener('change', () => {
  pref.fps = Number(ui.setFps.value);
  savePref();
  requestFps(pref.fps);
});
ui.setHud.addEventListener('change', () => {
  pref.hud = ui.setHud.checked;
  savePref();
  ui.hud.classList.toggle('hidden', !pref.hud);
});

/* 面板里显示当前真实状态（分辨率/帧率/端到端/丢帧） */
setInterval(() => {
  if (ui.settings.classList.contains('hidden')) return;
  const s = window.__ww?.status?.();
  if (!s) { ui.setNote.textContent = '未连接'; return; }
  ui.setNote.textContent = `${s.resolution} · ${s.fps} fps · ${s.e2eMs} ms` +
    (s.skipped ? ` · 丢帧 ${s.skipped}` : '');
}, 800);
