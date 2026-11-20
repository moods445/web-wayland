// 全功能回归测试：CDP 驱动真实页面，覆盖设置面板 + 输入 + 画面非黑屏
// 用法: node scripts/regression-test.mjs [cdpPort] [waitMs]
//   cdpPort 默认 9222；waitMs 默认 14000（等首帧与编码器重建）
// 用例说明（分类 / 前置 / 步骤 / 预期）：../docs/TEST-CASES.md
//   1) 基础健康     ../docs/TEST-CASES.md#b-连接与传输
//   2) 画面非黑屏   ../docs/TEST-CASES.md#c-画面渲染与清晰度
//   3~6) 设置面板   ../docs/TEST-CASES.md#g-设置面板与偏好
//   7) 输入链路     ../docs/TEST-CASES.md#e-输入链路
//   8) 鼠标端到端   ../docs/TEST-CASES.md#f-鼠标端到端测试
//   9) 运行错误     ../docs/TEST-CASES.md#i-稳定性与异常恢复
//  10) 音频链路     ../docs/TEST-CASES.md#k-音频
//  11) 串流策略     ../docs/TEST-CASES.md#l-串流策略
//  12) 色度 4:4:4   ../docs/TEST-CASES.md#m-色度采样-444
import { spawn, execFileSync } from 'node:child_process';
import fs from 'node:fs';

const port = Number(process.argv[2] || 9222);
const waitMs = Number(process.argv[3] || 14000);
const URL = 'https://127.0.0.1:8443/?auto=1&debug=1';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/* IDE 终端会注入 .trae-cn 的私有库，直接继承会让 chrome/hyprctl 报符号错误 */
const cleanEnv = { ...process.env };
delete cleanEnv.LD_LIBRARY_PATH;

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

/* ── 宿主机（Hyprland）侧断言 ──────────────────────────────
   鼠标是否真的动了、真的点到了，页面上看不出来，只能用 compositor 状态验证。 */
function hyprEnv() {
  if (process.env.HYPRLAND_INSTANCE_SIGNATURE) return cleanEnv;
  const runtime = process.env.XDG_RUNTIME_DIR || `/run/user/${process.getuid()}`;
  try {
    const sig = execFileSync('ls', ['-t', `${runtime}/hypr`], { env: cleanEnv, encoding: 'utf8' })
      .trim().split('\n')[0];
    if (sig) return { ...cleanEnv, HYPRLAND_INSTANCE_SIGNATURE: sig };
  } catch { /* 没有 compositor：下面的断言会给出明确失败信息 */ }
  return cleanEnv;
}
const hypr = (args) => execFileSync('hyprctl', args, { env: hyprEnv(), encoding: 'utf8' });
const cursorPos = () => {
  const [x, y] = hypr(['cursorpos']).trim().split(',').map(Number);
  return { x, y };
};

/* wwcap 抓的是哪个输出，就从 monitors 里取那个输出的逻辑尺寸做坐标换算 */
function wwcapOutput() {
  try {
    const pid = execFileSync('pgrep', ['-x', 'wwcap'], { env: cleanEnv, encoding: 'utf8' }).trim().split('\n')[0];
    const cmd = fs.readFileSync(`/proc/${pid}/cmdline`, 'utf8').split('\0');
    const i = cmd.indexOf('--output');
    if (i >= 0) return cmd[i + 1];
  } catch { /* 退回焦点输出 */ }
  const mons = JSON.parse(hypr(['monitors', '-j']));
  return (mons.find((m) => m.focused) || mons[0]).name;
}

async function runMouseTests({ send, evalJs }) {
  let mon, canvas;
  try {
    const out = wwcapOutput();
    mon = JSON.parse(hypr(['monitors', '-j'])).find((m) => m.name === out);
    canvas = await evalJs(`(() => {
      const r = document.getElementById('screen').getBoundingClientRect();
      return { l: r.left, t: r.top, w: r.width, h: r.height };
    })()`);
  } catch (e) {
    check('鼠标端到端测试环境可用', false, `hyprctl/输出不可用: ${e.message}`);
    return;
  }
  if (!mon || !canvas?.w) {
    check('鼠标端到端测试环境可用', false, `输出 ${mon?.name || '?'} / 画布尺寸缺失`);
    return;
  }

  const toClient = (nx, ny) => ({ x: canvas.l + nx * canvas.w, y: canvas.t + ny * canvas.h });
  const expectAt = (nx, ny) => ({ x: Math.round(mon.x + nx * mon.width), y: Math.round(mon.y + ny * mon.height) });
  const near = (a, b, tol = 6) => Math.abs(a.x - b.x) <= tol && Math.abs(a.y - b.y) <= tol;
  const dispatch = (type, p, extra) => send('Input.dispatchMouseEvent', { type, ...p, ...extra });
  const moveTo = (nx, ny) => dispatch('mouseMoved', toClient(nx, ny), { button: 'none', buttons: 0, clickCount: 0 });
  const clickAt = async (nx, ny) => {
    const p = toClient(nx, ny);
    await dispatch('mouseMoved', p, { button: 'none', buttons: 0, clickCount: 0 });
    await dispatch('mousePressed', p, { button: 'left', buttons: 1, clickCount: 1 });
    await dispatch('mouseReleased', p, { button: 'left', buttons: 0, clickCount: 1 });
  };

  /* 1) 真实 DOM mousemove → 远端光标位置：绝对坐标必须 1:1 对应，不能错位 */
  let moveOk = true, slowest = 0, moveDetail = '';
  for (const [nx, ny] of [[0.25, 0.25], [0.7, 0.6], [0.5, 0.5]]) {
    const want = expectAt(nx, ny);
    const t0 = Date.now();
    await moveTo(nx, ny);
    let got = null;
    while (Date.now() - t0 < 1500) {
      got = cursorPos();
      if (near(got, want)) break;
      await sleep(20);
    }
    slowest = Math.max(slowest, Date.now() - t0);
    if (!got || !near(got, want)) {
      moveOk = false;
      moveDetail = `(${nx},${ny}) got ${got?.x},${got?.y} want ${want.x},${want.y}`;
      break;
    }
  }
  check('鼠标移动位置准确（无错位）', moveOk, moveDetail || `最慢 ${slowest}ms`);
  check('鼠标移动响应及时（不卡顿）', moveOk && slowest < 400, `最慢 ${slowest}ms`);

  /* 2) 连续移动：每帧一个点扫过整屏，光标必须单调推进且到达终点（卡死会停在中途） */
  const xs = [];
  const N = 30;
  for (let i = 0; i < N; i++) {
    await moveTo(0.15 + (0.7 * i) / (N - 1), 0.5);
    await sleep(16);
    if (i % 8 === 0) xs.push(cursorPos().x);
  }
  await sleep(200);
  const sweepEnd = cursorPos();
  const endWant = expectAt(0.85, 0.5);
  const monotonic = xs.every((v, i) => i === 0 || v >= xs[i - 1] - 4);
  check('连续移动无停顿（轨迹单调推进）', monotonic && xs.length >= 3, xs.join(' → '));
  check('连续移动到达终点', near(sweepEnd, endWant, 8), `got ${sweepEnd.x},${sweepEnd.y} want ${endWant.x},${endWant.y}`);

  /* 3) 点击：开两个测试窗口，点谁谁获得焦点。焦点变化说明 compositor 在正确位置收到了按键 */
  let wins = [];
  try {
    for (const t of ['wwtest-left', 'wwtest-right']) hypr(['dispatch', 'exec', `foot -T ${t}`]);
    for (let i = 0; i < 60; i++) {
      wins = JSON.parse(hypr(['clients', '-j'])).filter((c) => c.title?.startsWith('wwtest-'));
      if (wins.length >= 2) break;
      await sleep(100);
    }
    if (wins.length < 2) {
      check('点击测试窗口就绪', false, `只找到 ${wins.length} 个测试窗口`);
      return;
    }
    wins.sort((a, b) => a.at[0] - b.at[0]);
    const [left, right] = wins;
    const activeAddr = () => JSON.parse(hypr(['activewindow', '-j']))?.address || '';
    const winNorm = (w) => [
      (w.at[0] + w.size[0] / 2 - mon.x) / mon.width,
      (w.at[1] + w.size[1] / 2 - mon.y) / mon.height,
    ];

    await clickAt(...winNorm(right));
    await sleep(400);
    check('点击命中右侧窗口', activeAddr() === right.address, `${activeAddr()} vs ${right.address}`);

    await clickAt(...winNorm(left));
    await sleep(400);
    check('点击命中左侧窗口', activeAddr() === left.address, `${activeAddr()} vs ${left.address}`);

    /* 4) 同一帧内「移动 + 按下」：pb 必须落在按下点，不能用到上一帧的旧坐标 */
    const p = toClient(...winNorm(right));
    await dispatch('mouseMoved', p, { button: 'none', buttons: 0, clickCount: 0 });
    await dispatch('mousePressed', p, { button: 'left', buttons: 1, clickCount: 1 });
    await dispatch('mouseReleased', p, { button: 'left', buttons: 0, clickCount: 1 });
    await sleep(500);
    check('快速移动后点击不落到旧位置', activeAddr() === right.address, `${activeAddr()} vs ${right.address}`);
  } finally {
    for (const w of wins) {
      try { process.kill(w.pid, 'SIGTERM'); } catch { /* 已退出 */ }
    }
  }
}



const chrome = spawn('google-chrome', [
  '--headless=new', '--no-sandbox', `--remote-debugging-port=${port}`,
  /* 无头环境没有用户手势，AudioContext 会一直 suspended；
     关掉自动播放限制，音频用例才能验证到「解码 + 播放时钟」而不是只验证收包 */
  '--autoplay-policy=no-user-gesture-required',
  '--window-size=1920,1080', '--user-data-dir=/tmp/ww-reg-profile', URL,
], { stdio: 'ignore', detached: true, env: cleanEnv });

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

  // 5b) 串流策略：点「流畅优先」后状态与帧率选项联动，再切回平衡
  const strat = await evalJs(`(() => {
    const btn=document.querySelector('#set-strategy button[data-strategy="1"]');
    btn.click();
    const p=JSON.parse(localStorage.getItem('ww.pref')||'{}');
    const opts=[...document.getElementById('set-fps').options].map(o=>o.value);
    return { pref:p.strategy, live:window.__ww.status().strategy, opts };
  })()`);
  check('流畅优先策略生效', strat.pref === 1 && strat.live === 1 &&
    strat.opts.length === 2 && strat.opts.includes('60') && strat.opts.includes('30'),
    JSON.stringify(strat));
  await evalJs(`document.querySelector('#set-strategy button[data-strategy="0"]').click()`);
  await sleep(400);
  const stratBack = await evalJs(`(() => {
    const p=JSON.parse(localStorage.getItem('ww.pref')||'{}');
    return { pref:p.strategy, live:window.__ww.status().strategy };
  })()`);
  check('切回平衡策略', stratBack.pref === 0 && stratBack.live === 0, JSON.stringify(stratBack));

  // 5c) 4:4:4：状态可观测，置灰时必须给出原因
  const chroma = await evalJs(`(() => {
    const s=window.__ww.status(), cb=document.getElementById('set-chroma');
    return { chroma:s.chroma, decode444:s.decode444, disabled:cb.disabled,
             note:document.getElementById('set-chroma-note').textContent };
  })()`);
  check('色度采样状态可观测', ['4:2:0','4:4:4','—'].includes(chroma.chroma), JSON.stringify(chroma));
  check('4:4:4 开关与能力一致', !chroma.disabled || chroma.note.length > 0, JSON.stringify(chroma));

  // 6) HUD 开关
  const hudOk = await evalJs(`(() => {
    const s=document.getElementById('set-hud'), h=document.getElementById('hud');
    s.checked=false; s.dispatchEvent(new Event('change'));
    const hidden=h.classList.contains('hidden');
    s.checked=true; s.dispatchEvent(new Event('change'));
    return hidden && !h.classList.contains('hidden');
  })()`);
  check('HUD 开关可切换', hudOk === true);

  // 7) 统计信息：默认关闭、开关生效、网关 ping/pong 提供 RTT
  const statsDefault = await evalJs(`(() => {
    const p=JSON.parse(localStorage.getItem('ww.pref')||'{}');
    return { stored: p.stats, hidden: document.getElementById('stats').classList.contains('hidden') };
  })()`);
  check('统计信息默认关闭', statsDefault.hidden === true, JSON.stringify(statsDefault));
  const statsOn = await evalJs(`(() => {
    const s=document.getElementById('set-stats'), p=document.getElementById('stats');
    s.checked=true; s.dispatchEvent(new Event('change'));
    return { visible: !p.classList.contains('hidden'), pref: JSON.parse(localStorage.getItem('ww.pref')||'{}').stats };
  })()`);
  check('统计信息开关生效', statsOn.visible === true && statsOn.pref === true, JSON.stringify(statsOn));
  await sleep(2600);
  const statsData = await evalJs(`(() => {
    const rows=[...document.querySelectorAll('#stats .stat-row')];
    const get=(label)=>rows.find(r=>r.querySelector('.stat-label')?.textContent.startsWith(label))?.querySelector('.stat-value')?.textContent;
    return { stream:get('Video stream'), rtt:get('Average network latency'), rttMs:window.__ww.status().rttMs };
  })()`);
  check('统计面板有实时数据', /FPS/.test(statsData.stream || '') && statsData.stream !== '-', JSON.stringify(statsData));
  check('网关 ping/pong 返回 RTT', typeof statsData.rttMs === 'number' && statsData.rttMs >= 0,
    `rtt=${statsData.rttMs} text=${statsData.rtt}`);

  // 7b) 音频链路：音频包到达 + Opus 解码产出（headless 下也可能无声，所以不断言扬声器状态）
  await sleep(1500);
  const au = await evalJs(`(() => {
    const s = window.__ww.status();
    return { packets: s.audioPackets, decoded: s.audioDecoded, state: s.audioState,
             bufferMs: s.audioBufferMs, recovered: s.audioRecovered, err: s.audioError };
  })()`);
  check('音频包已到达浏览器', au.packets > 0, `packets=${au.packets}`);
  check('Opus 解码器已产出 PCM', au.decoded > 0,
    `decoded=${au.decoded} state=${au.state} buf=${au.bufferMs}ms err=${au.err || '-'}`);

  // 8) 快捷键帮助：设置里打开、关闭按钮可关
  const helpOk = await evalJs(`(() => {
    const help=document.getElementById('help');
    document.getElementById('set-help').click();
    const opened=!help.classList.contains('hidden');
    document.getElementById('help-close').click();
    return opened && help.classList.contains('hidden');
  })()`);
  check('快捷键帮助可打开与关闭', helpOk === true);
  /* 关掉统计，避免持久化偏好影响下次「默认关闭」断言 */
  await evalJs(`(() => { const s=document.getElementById('set-stats'); s.checked=false; s.dispatchEvent(new Event('change')); })()`);

  // 9) 输入链路：__ww.pointer/key 不抛错且服务端有响应（工作区切换）
  let inputOk = true;
  try {
    await evalJs('window.__ww.pointer(0.5,0.5)');
    await evalJs('window.__ww.key(125,1); window.__ww.key(3,1); window.__ww.key(3,0); window.__ww.key(125,0)');
    await sleep(300);
  } catch { inputOk = false; }
  check('输入注入接口无异常', inputOk);

  /* F. 鼠标端到端：真实 DOM 事件走完 浏览器→网关→wwcap→Hyprland，
       用 compositor 光标坐标与窗口焦点验证「流畅、能点、不错位」 */
  console.log('\n── F. 鼠标端到端测试 ──');
  await runMouseTests({ send, evalJs });

  // 8) 恢复自动帧率：否则测试设的 30fps 会留在 wwcap 里，影响后续手动使用
  await evalJs(`(() => { const s=document.getElementById('set-fps'); s.value='0'; s.dispatchEvent(new Event('change')); })()`);
  await sleep(500);

  // 9) 无 JS 运行错误
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
