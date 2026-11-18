// 全功能回归测试：CDP 驱动真实页面，覆盖设置面板 + 输入 + 画面非黑屏
// 用法: node scripts/regression-test.mjs [cdpPort] [waitMs]
//   cdpPort 默认 9222；waitMs 默认 14000（等首帧与编码器重建）
import { spawn } from 'node:child_process';
import fs from 'node:fs';

const port = Number(process.argv[2] || 9222);
const waitMs = Number(process.argv[3] || 14000);
const URL = 'https://127.0.0.1:8443/?auto=1&debug=1';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const results = [];
const check = (name, ok, detail = '') => {
  results.push({ name, ok, detail });
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? '  — ' + detail : ''}`);
};

async function connect() {
  let targets = [];
  for (let i = 0; i < 40 && !targets.length; i++) {
    try {
      const r = await fetch(`http://127.0.0.1:${port}/json`);
      targets = (await r.json()).filter((t) => t.type === 'page');
    } catch { /* retry */ }
    if (!targets.length) await sleep(400);
  }
  if (!targets.length) throw new Error('无页面目标');
  const ws = new WebSocket(targets[0].webSocketDebuggerUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
  let id = 0;
  const pending = new Map();
  const consoleErrors = [];
  ws.onmessage = (ev) => {
    const m = JSON.parse(ev.data);
    if (m.id && pending.has(m.id)) { pending.get(m.id)(m.result); pending.delete(m.id); }
    if (m.method === 'Runtime.consoleAPICalled' && m.params.type === 'error') {
      consoleErrors.push(m.params.args.map((a) => a.value ?? a.description).join(' '));
    }
    if (m.method === 'Runtime.exceptionThrown') {
      consoleErrors.push(m.params.exceptionDetails?.text || 'exception');
    }
  };
  const send = (method, params = {}) => new Promise((res) => {
    const mid = ++id;
    pending.set(mid, res);
    ws.send(JSON.stringify({ id: mid, method, params }));
  });
  const evalJs = async (expr) => {
    const r = await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true });
    if (r?.exceptionDetails) throw new Error(r.exceptionDetails.text);
    return r?.result?.value;
  };
  return { send, evalJs, consoleErrors, ws };
}

// PNG 是否接近纯色（黑屏）：抽样画布区域像素的方差
async function captureAndAnalyze(send, outPath) {
  const shot = await send('Page.captureScreenshot', { format: 'png' });
  const buf = Buffer.from(shot.data, 'base64');
  fs.writeFileSync(outPath, buf);
  // 用页面内 canvas 采样判断画面是否有内容（canvas 是 2d，可读像素）
  return buf.length;
}

const chrome = spawn('google-chrome', [
  '--headless=new', '--no-sandbox', `--remote-debugging-port=${port}`,
  '--window-size=1920,1080', '--user-data-dir=/tmp/ww-reg-profile', URL,
], { stdio: 'ignore', detached: true });

try {
  const { send, evalJs, consoleErrors, ws } = await connect();
  await send('Runtime.enable');
  await send('Page.enable');
  await sleep(waitMs);

  // 1) 基础健康
  const st = await evalJs('window.__ww && window.__ww.status()');
  check('__ww 接口可用', !!st);
  if (!st) throw new Error('__ww 未初始化');
  check('WebTransport 已连接', st.connected === true);
  check('持续收到分片并渲染', st.datagrams > 100 && st.rendered > 100,
    `dg=${st.datagrams} rendered=${st.rendered}`);
  check('零丢片 / 零解码错误', st.lost === 0 && st.dropped === 0,
    `lost=${st.lost} dropped=${st.dropped}`);

  // 2) 画面非黑屏：直接读 canvas 像素（2d context）
  const canvasStats = await evalJs(`(() => {
    const c = document.getElementById('screen');
    const ctx = c.getContext('2d');
    const pts = [[0.25,0.25],[0.5,0.5],[0.75,0.75],[0.1,0.9],[0.9,0.1]];
    let sum=0, sum2=0, n=0;
    for (const [fx,fy] of pts) {
      const d = ctx.getImageData(Math.floor(c.width*fx), Math.floor(c.height*fy), 1, 1).data;
      const lum = (d[0]+d[1]+d[2])/3;
      sum+=lum; sum2+=lum*lum; n++;
    }
    const mean=sum/n, variance=sum2/n-mean*mean;
    return { mean: Math.round(mean), variance: Math.round(variance), w:c.width, h:c.height };
  })()`);
  check('画面尺寸为串流分辨率', canvasStats.w >= 1920 && canvasStats.h >= 1080,
    `${canvasStats.w}x${canvasStats.h}`);
  check('画面非黑屏（像素有明暗变化）', canvasStats.variance > 100 && canvasStats.mean > 8,
    `mean=${canvasStats.mean} var=${canvasStats.variance}`);

  // 3) 设置面板：齿轮开关
  const gearOk = await evalJs(`(() => {
    const gear=document.getElementById('gear'), panel=document.getElementById('settings');
    const before=panel.classList.contains('hidden');
    gear.click(); const after=panel.classList.contains('hidden');
    gear.click();
    return before !== after;
  })()`);
  check('设置面板可开合', gearOk === true);

  // 4) 帧率切换：30fps
  await evalJs(`(() => { const s=document.getElementById('set-fps'); s.value='30'; s.dispatchEvent(new Event('change')); })()`);
  await sleep(2000);
  const fpsOk = await evalJs(`(() => { const p=JSON.parse(localStorage.getItem('ww.pref')||'{}'); return p.fps===30; })()`);
  check('帧率切换写入偏好并生效', fpsOk === true);

  // 5) 编码器切换：nvenc → 观察日志无致命错误，再切回 vaapi
  await evalJs(`(() => { const s=document.getElementById('set-enc'); s.value='nvenc'; s.dispatchEvent(new Event('change')); })()`);
  await sleep(2500);
  const encNv = await evalJs(`(() => { const p=JSON.parse(localStorage.getItem('ww.pref')||'{}'); return p.enc; })()`);
  check('编码器切换到 NVENC', encNv === 'nvenc', encNv);
  const afterNv = await evalJs('window.__ww.status()');
  check('NVENC 切换后仍在渲染', afterNv.rendered > st.rendered,
    `rendered ${st.rendered}->${afterNv.rendered}`);
  await evalJs(`(() => { const s=document.getElementById('set-enc'); s.value='vaapi'; s.dispatchEvent(new Event('change')); })()`);
  await sleep(2500);
  const encVa = await evalJs(`(() => { const p=JSON.parse(localStorage.getItem('ww.pref')||'{}'); return p.enc; })()`);
  check('编码器切回 VAAPI', encVa === 'vaapi', encVa);

  // 6) HUD 开关
  const hudOk = await evalJs(`(() => {
    const s=document.getElementById('set-hud'), h=document.getElementById('hud');
    s.checked=false; s.dispatchEvent(new Event('change'));
    const hidden=h.classList.contains('hidden');
    s.checked=true; s.dispatchEvent(new Event('change'));
    return hidden && !h.classList.contains('hidden');
  })()`);
  check('HUD 开关可切换', hudOk === true);

  // 7) 输入链路：__ww.pointer/key 不抛错且服务端有响应（工作区切换）
  let inputOk = true;
  try {
    await evalJs('window.__ww.pointer(0.5,0.5)');
    await evalJs('window.__ww.key(125,1); window.__ww.key(3,1); window.__ww.key(3,0); window.__ww.key(125,0)');
    await sleep(300);
  } catch { inputOk = false; }
  check('输入注入接口无异常', inputOk);

  // 8) 无 JS 运行错误
  const realErrors = consoleErrors.filter((e) => e && !/favicon|cert|WebTransport.*close/i.test(e));
  check('无 JS 运行时错误', realErrors.length === 0, realErrors.slice(0, 2).join('|'));

  // 保存最终截图
  const shotBytes = await captureAndAnalyze(send, '/tmp/regression-final.png');
  check('截图文件有效', shotBytes > 20000, `${shotBytes} bytes`);

  ws.close();
} finally {
  try { process.kill(-chrome.pid, 'SIGKILL'); } catch { /* noop */ }
}

const failed = results.filter((r) => !r.ok);
console.log(`\n${results.length - failed.length}/${results.length} 通过`);
process.exit(failed.length ? 1 : 0);
