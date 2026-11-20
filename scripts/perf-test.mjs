// 分辨率帧率实测：逐档切换分辨率，采样前端渲染 fps 与服务端耗时
// 用法: node scripts/perf-test.mjs [cdpPort]
// 用例说明（分类 / 前置 / 步骤 / 预期）：../docs/TEST-CASES.md
//   性能与延迟   ../docs/TEST-CASES.md#h-性能与延迟
//   分辨率协商   ../docs/TEST-CASES.md#d-分辨率协商
import { spawn } from 'node:child_process';

const port = Number(process.argv[2] || 9224);
const URL = 'https://127.0.0.1:8443/?auto=1';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

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
  ws.onmessage = (ev) => {
    const m = JSON.parse(ev.data);
    if (m.id && pending.has(m.id)) { pending.get(m.id)(m.result); pending.delete(m.id); }
  };
  const send = (method, params = {}) => new Promise((res) => {
    const mid = ++id; pending.set(mid, res);
    ws.send(JSON.stringify({ id: mid, method, params }));
  });
  const evalJs = async (expr) => {
    const r = await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true });
    return r?.result?.value;
  };
  return { send, evalJs, ws };
}

const chrome = spawn('google-chrome', [
  '--headless=new', '--no-sandbox', `--remote-debugging-port=${port}`,
  '--window-size=1920,1080', '--user-data-dir=/tmp/ww-perf-profile', URL,
], { stdio: 'ignore', detached: true });

try {
  const { evalJs, ws } = await connect();
  await sleep(10000);
  // 关闭 HUD 定时开销无关；直接采样
  for (const res of ['1920x1080', '2560x1440', '3200x1800', '3840x2160']) {
    await evalJs(`(() => { const s=document.getElementById('set-res'); s.value='${res}'; s.dispatchEvent(new Event('change')); })()`);
    await sleep(6000); // 等编码器重建 + 稳定
    // 采样 3 次 fps 取平均
    let fpsSum = 0, resNow = '', rendered = 0;
    for (let i = 0; i < 3; i++) {
      const st = await evalJs('window.__ww.status()');
      if (st) { fpsSum += st.fps; resNow = st.resolution; rendered = st.rendered; }
      await sleep(1000);
    }
    console.log(`${res.padEnd(10)} -> 实际 ${String(resNow).padEnd(11)} 前端fps≈${(fpsSum/3).toFixed(1)}  rendered=${rendered}`);
  }
  ws.close();
} finally {
  try { process.kill(-chrome.pid, 'SIGKILL'); } catch { /* noop */ }
}
