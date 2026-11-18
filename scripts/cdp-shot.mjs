// 用 CDP 对运行中的 Chrome 截图，验证实时串流画面
const port = process.argv[2] || '9222';
const out = process.argv[3] || '/tmp/cdp-shot.png';
const waitMs = Number(process.argv[4] || 8000);

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function main() {
  let targets = [];
  for (let i = 0; i < 20 && !targets.length; i++) {
    try {
      const r = await fetch(`http://127.0.0.1:${port}/json`);
      targets = (await r.json()).filter((t) => t.type === 'page');
    } catch { /* retry */ }
    if (!targets.length) await sleep(500);
  }
  if (!targets.length) throw new Error('没有可用的页面目标');

  const ws = new WebSocket(targets[0].webSocketDebuggerUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });

  let id = 0;
  const pending = new Map();
  ws.onmessage = (ev) => {
    const msg = JSON.parse(ev.data);
    if (msg.id && pending.has(msg.id)) {
      pending.get(msg.id)(msg.result);
      pending.delete(msg.id);
    }
  };
  const send = (method, params = {}) => new Promise((res) => {
    const mid = ++id;
    pending.set(mid, res);
    ws.send(JSON.stringify({ id: mid, method, params }));
  });

  await send('Page.enable');
  await send('Runtime.enable');
  await sleep(waitMs);

  // 读取页面内的调试计数器
  const evalRes = await send('Runtime.evaluate', {
    expression: "document.getElementById('stage')?.innerText || 'no-stage'",
    returnByValue: true,
  });
  console.log('page:', JSON.stringify(evalRes?.result?.value ?? ''));

  if (process.env.EXPR) {
    const r = await send('Runtime.evaluate', {
      expression: process.env.EXPR,
      awaitPromise: true,
      returnByValue: true,
    });
    console.log('expr:', JSON.stringify(r?.result?.value ?? r));
  }

  const shot = await send('Page.captureScreenshot', { format: 'png' });
  const fs = await import('node:fs');
  fs.writeFileSync(out, Buffer.from(shot.data, 'base64'));
  console.log('saved', out);
  ws.close();
}

main().catch((e) => { console.error('CDP 失败:', e.message); process.exit(1); });
