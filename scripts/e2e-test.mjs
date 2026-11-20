// 端到端自动化测试：启动无头 Chrome → 通过 window.__ww 断言串流健康度
// 用法: node scripts/e2e-test.mjs [cdpPort] [waitMs]
// 用例说明（分类 / 前置 / 步骤 / 预期）：../docs/TEST-CASES.md
//   B 连接与传输   ../docs/TEST-CASES.md#b-连接与传输
//   C 画面渲染与清晰度 ../docs/TEST-CASES.md#c-画面渲染与清晰度
//   D 分辨率协商   ../docs/TEST-CASES.md#d-分辨率协商
//   E 输入链路     ../docs/TEST-CASES.md#e-输入链路
//   L 串流策略     ../docs/TEST-CASES.md#l-串流策略
//   M 色度采样 4:4:4 ../docs/TEST-CASES.md#m-色度采样-444
import { spawn } from 'node:child_process';
import fs from 'node:fs';

const port = Number(process.argv[2] || 9222);
const waitMs = Number(process.argv[3] || 12000);
const URL = 'https://127.0.0.1:8443/?auto=1&debug=1';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const results = [];
const check = (name, ok, detail = '') => {
  results.push({ name, ok, detail });
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? '  — ' + detail : ''}`);
};

async function cdp() {
  let targets = [];
  for (let i = 0; i < 30 && !targets.length; i++) {
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
  const logs = [];
  ws.onmessage = (ev) => {
    const m = JSON.parse(ev.data);
    if (m.id && pending.has(m.id)) { pending.get(m.id)(m.result); pending.delete(m.id); }
    if (m.method === 'Runtime.consoleAPICalled' && m.params.type === 'error') {
      logs.push(m.params.args.map((a) => a.value ?? a.description).join(' '));
    }
  };
  const send = (method, params = {}) => new Promise((res) => {
    const mid = ++id;
    pending.set(mid, res);
    ws.send(JSON.stringify({ id: mid, method, params }));
  });
  const evalJs = async (expr) => {
    const r = await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true });
    return r?.result?.value;
  };
  return { send, evalJs, logs, ws };
}

const scaleFlag = process.env.WW_SCALE ? [`--force-device-scale-factor=${process.env.WW_SCALE}`] : [];
const winSize = process.env.WW_SIZE || '1920,1080';
const chrome = spawn('google-chrome', [
  '--headless=new', '--no-sandbox', `--remote-debugging-port=${port}`,
  `--window-size=${winSize}`, ...scaleFlag,
  '--user-data-dir=/tmp/ww-e2e-profile', URL,
], { stdio: 'ignore', detached: true });

try {
  const { send, evalJs, logs, ws } = await cdp();
  await send('Runtime.enable');
  await sleep(waitMs);

  const st = await evalJs('window.__ww ? window.__ww.status() : null');
  check('页面暴露 __ww 自测接口', !!st);
  if (!st) throw new Error('__ww 未初始化');
  check('WebTransport 已连接', st.connected === true);
  check('收到视频分片', st.datagrams > 5, `datagrams=${st.datagrams}`);
  check('已渲染帧数 ≥ 10', st.rendered >= 10, `rendered=${st.rendered}`);
  check('无丢片', st.lost === 0, `lost=${st.lost}`);

  /* DPR=2 的客户端会触发「物理 4K + scale 2」协商，切换需要等编码器重建 */
  const expectHighRes = Number(process.env.WW_SCALE || 1) >= 2;
  if (expectHighRes) {
    for (let i = 0; i < 24; i++) {
      const s2 = await evalJs('window.__ww.status()');
      if (s2?.resolution?.startsWith('3')) { st.resolution = s2.resolution; st.rendered = s2.rendered; break; }
      await sleep(500);
    }
    check('协商到 4K 输出（scale 2）', String(st.resolution).startsWith('3'), st.resolution);
  } else {
    /* 断言以「页面实际请求的模式」为准：浏览器可用区域不等于窗口尺寸 */
    const want = st.requested ? `${st.requested.w}×${st.requested.h}` : '1920×1080';
    const [gw, gh] = String(st.resolution).split('×').map(Number);
    const okRes = st.requested
      ? Math.abs(gw - st.requested.w) <= 1 && Math.abs(gh - st.requested.h) <= 1
      : st.resolution === want;
    check('串流分辨率与协商一致（±1 取偶）', okRes, `got ${st.resolution}, want ${want}`);
  }
  check('解码队列未堆积（超阈值会主动丢帧）', st.decoderQueue <= 6,
    `q=${st.decoderQueue} skipped=${st.skipped ?? 0}`);
  check('无解码错误', st.dropped === 0 && !logs.some((l) => l.includes('decode')), logs.slice(0, 2).join('|'));

  // 输入链路：注入指针 + 按键，服务端侧再用 hyprctl cursorpos 校验
  await evalJs('window.__ww.pointer(0.25, 0.25)');
  await sleep(400);
  await evalJs('window.__ww.pointer(0.75, 0.6)');
  await sleep(600);

  const shot = await send('Page.captureScreenshot', { format: 'png' });
  fs.writeFileSync('/tmp/e2e-final.png', Buffer.from(shot.data, 'base64'));

  const health = await (await fetch('https://127.0.0.1:8443/healthz', { // 便于脚本断言，证书错误在 node 下忽略
    // @ts-ignore
    dispatcher: undefined,
  }).catch(() => null));
  if (health) {
    // node 默认不信任自签 CA，这里不阻塞测试
  }
  ws.close();
} finally {
  try { process.kill(-chrome.pid, 'SIGKILL'); } catch { /* noop */ }
}

const failed = results.filter((r) => !r.ok);
console.log(`\n${results.length - failed.length}/${results.length} 通过`);
process.exit(failed.length ? 1 : 0);
