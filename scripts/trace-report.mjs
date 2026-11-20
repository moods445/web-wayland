// 输入追踪报告：读前端「导出追踪」得到的 JSON（客户端 + 服务端合并），
// 把「输入发出 → 服务端注入 → 画面到达 → 上屏」拆开，定位鼠标卡在哪一段。
//
// 用法：
//   node scripts/trace-report.mjs ww-trace-2026-11-20T09-12-33-123Z.json
//   node scripts/trace-report.mjs trace.json --worst 20     # 只列最差的 N 条
//
// 时钟对齐：客户端每条输入都带 performance.now()（ts），服务端记录收到时的
// 单调时钟。两边无法直接比较，用 min(server_recv_ms - ts) 当作基线偏移，
// 它吸收了「单程网络延迟 + 时钟差」，因此算出的 up 是**相对最小值的额外延迟**，
// 而不是绝对单程延迟（单程需要 RTT/2 假设，误差同量级）。
import fs from 'node:fs';

// 不传文件时直接读服务端落盘的自动上传结果（前端每 15s 追加一份，无需人工导出）：
//   /tmp/ww-client-trace.jsonl  客户端：一行一条（snap / ev / pair）
//   /tmp/wwcap-trace.jsonl      服务端：一行一条（in / frm）
const CLIENT_DEFAULT = process.env.WW_TRACE_DIR
  ? `${process.env.WW_TRACE_DIR}/ww-client-trace.jsonl` : '/tmp/ww-client-trace.jsonl';
const SERVER_DEFAULT = process.env.WW_TRACE_DIR
  ? `${process.env.WW_TRACE_DIR}/wwcap-trace.jsonl` : '/tmp/wwcap-trace.jsonl';

const arg = process.argv[2];
const file = arg && !arg.startsWith('--') ? arg : null;
const worstN = Number(process.argv[process.argv.indexOf('--worst') + 1]) || 10;

/* NDJSON 逐行解析；轮转可能把最后一行截断，解析失败的行直接跳过 */
function parseNdjson(text) {
  const out = [];
  for (const line of text.split('\n')) {
    if (!line) continue;
    try { out.push(JSON.parse(line)); } catch { /* 残行 */ }
  }
  return out;
}

/* 把文件切成若干 snap 块（snap + 其后的 ev/pair，直到下一个 snap）。
   注意不能简单取"最后一个块"：每个块只保留最近 30 秒事件，如果用户最后 30 秒
   没动鼠标，那个块里就一条 pm 都没有，分析会误判"数据不足"。 */
function clientFromNdjson(text) {
  const recs = parseNdjson(text);
  const blocks = [];
  let cur = null;
  for (const r of recs) {
    if (r.type === 'snap') {
      cur = { head: r, events: [], pairs: [] };
      blocks.push(cur);
    } else if (cur) {
      if (r.type === 'ev') {
        cur.events.push({
          k: r.kind, t: r.t, x: r.x, y: r.y, v: r.v, k2: r.key, d: r.down,
          id: r.id, pts: r.pts, n: r.n,   // 帧事件需要 pts/id 才能和服务端按帧对齐
        });
      } else if (r.type === 'pair') {
        cur.pairs.push({ t: r.t, lag: r.lag });
      }
    }
  }
  if (!blocks.length) throw new Error('jsonl 里找不到 snap 记录（客户端还没上传过）');
  /* 选"指针样本最多"的块（并列取较新的），这样分析窗口一定是用户真正操作过的那段 */
  const score = (b) => b.events.filter((e) => e.k === 'pm').length;
  let best = blocks[0];
  for (const b of blocks) if (score(b) >= score(best)) best = b;
  const b = best;
  return {
    takenAt: b.head.takenAt,
    meta: b.head.meta,
    stats: b.head.stats,
    events: b.events,
    pairs: b.pairs,
    picked: { blocks: blocks.length, pm: score(b) },
  };
}

function serverFromNdjson(text) {
  const recs = parseNdjson(text);
  return {
    v: 1,
    side: 'server',
    inputs: recs.filter((r) => r.type === 'in'),
    frames: recs.filter((r) => r.type === 'frm'),
  };
}

let data;
if (file) {
  data = JSON.parse(fs.readFileSync(file, 'utf8'));
} else {
  if (!fs.existsSync(CLIENT_DEFAULT)) {
    console.error(`没有找到 ${CLIENT_DEFAULT}`);
    console.error('（浏览器连上串流后会自动追加；也可手动导出后用 trace-report.mjs <file> 分析）');
    process.exit(2);
  }
  const client = clientFromNdjson(fs.readFileSync(CLIENT_DEFAULT, 'utf8'));
  const server = fs.existsSync(SERVER_DEFAULT)
    ? serverFromNdjson(fs.readFileSync(SERVER_DEFAULT, 'utf8')) : null;
  data = { v: 1, takenAt: client.takenAt, client, server };
  const kb = (p) => (fs.existsSync(p) ? `${(fs.statSync(p).size / 1024).toFixed(0)}KB` : '-');
  console.log(`读取: ${CLIENT_DEFAULT} (${kb(CLIENT_DEFAULT)}) + ${SERVER_DEFAULT} (${kb(SERVER_DEFAULT)})`);
  const age = (Date.now() - Date.parse(client.takenAt || 0)) / 1000;
  if (Number.isFinite(age) && age > 60) {
    console.log(`注意: 客户端快照已过期 ${age.toFixed(0)} 秒（浏览器可能已断开）`);
  }
}
const c = data.client;
const s = data.server;

const f = (v, n = 1) => (v == null || Number.isNaN(v) ? '-' : Number(v).toFixed(n));
const pct = (arr, p) => {
  if (!arr.length) return null;
  const a = arr.slice().sort((x, y) => x - y);
  return a[Math.min(a.length - 1, Math.floor((a.length - 1) * p))];
};

console.log('=== 追踪文件 ===');
console.log('导出时间:', data.takenAt || '(未知)');
const meta = c?.meta || {};
console.log('分辨率:', meta.resolution || '-', '| 编码器:', meta.enc || '-',
  '| 策略:', meta.strategy ?? '-', '| 目标帧率:', meta.fps ?? '-');
console.log('客户端:', String(meta.ua || '').slice(0, 70));

if (!c) {
  console.error('文件里没有 client 段，无法分析');
  process.exit(1);
}

const cs = c.stats || {};
console.log('\n=== 客户端 ===');
console.log(`时长 ${f(cs.durSec)}s | 输入 ${cs.inputs} 条 (${f(cs.inputRate)}/s) | 画面 ${cs.frames} 帧 (${f(cs.frameRate)}/s)`);
console.log(`事件缓冲 ${cs.events} 条（丢弃 ${cs.dropped}）`);
const lag = cs.inputLag;
if (lag) {
  console.log(`输入→上屏  p50 ${f(lag.p50)}ms  p95 ${f(lag.p95)}ms  p99 ${f(lag.p99)}ms  max ${f(lag.max)}ms  avg ${f(lag.avg)}ms  (n=${lag.n})`);
} else {
  console.log('输入→上屏  (没有指针输入样本)');
}
console.log(`帧到达间隔 >50ms: ${cs.frameGapsOver50ms ?? 0} 次，最长 ${f(cs.maxFrameGapMs)}ms`);

const st = meta.status || {};
if (st.decodeMs != null || st.queueMs != null) {
  console.log(`解码 ${f(st.decodeMs)}ms | 队列 ${f(st.queueMs)}ms | e2e ${f(st.e2eMs)}ms | RTT ${f(st.rttMs)}ms | 分辨率 ${st.resolution || '-'}`);
}

/* ── 服务端 ── */
let sin = [];
if (s && Array.isArray(s.inputs)) {
  sin = s.inputs.filter((e) => e.ts > 0);
}

console.log('\n=== 服务端 ===');
if (!s) {
  console.log('没有 server 段（网关 /trace 未取到，或 wwcap 未落盘）');
} else {
  console.log(`输入 ${s.inputs?.length ?? 0} 条，出帧 ${s.frames?.length ?? 0} 帧`);
  const inj = (s.inputs || []).map((e) => e.d / 1000);
  if (inj.length) {
    console.log(`收到→注入完成  p50 ${f(pct(inj, 0.5), 3)}ms  p95 ${f(pct(inj, 0.95), 3)}ms  max ${f(Math.max(...inj), 3)}ms`);
  }
  const fr = s.frames || [];
  if (fr.length > 1) {
    const iv = [];
    for (let i = 1; i < fr.length; i++) iv.push((fr[i].t - fr[i - 1].t) / 1000);
    console.log(`服务端出帧间隔  p50 ${f(pct(iv, 0.5))}ms  p95 ${f(pct(iv, 0.95))}ms  max ${f(Math.max(...iv))}ms`);
    console.log(`有效帧率 ≈ ${f(1000 / Math.max(pct(iv, 0.5), 0.001))} fps`);
    const kb = fr.reduce((a, e) => a + e.n, 0) / 1024 / Math.max((fr[fr.length - 1].t - fr[0].t) / 1e6, 0.001);
    console.log(`平均码率 ≈ ${f(kb * 8 / 1024)} Mbps`);
  }
}

/* ── 归因：把每条输入的三段拼起来 ── */
const pairs = c.pairs || [];
if (pairs.length && sin.length) {
  const byTs = new Map();
  for (const e of sin) byTs.set(e.ts.toFixed(3), e);

  /* 先对齐，再用**本会话内**的最小偏移做基线：服务端缓冲里可能混着别的客户端
     的输入（例如上一次测试或另一个标签页），拿全局 min 当基线会把 up 算成几万毫秒。 */
  const matched = [];
  for (const p of pairs) {
    const se = byTs.get(p.t.toFixed(3));
    if (se) matched.push({ p, se, off: se.t / 1000 - se.ts });
  }
  const upBase = matched.length ? Math.min(...matched.map((m) => m.off)) : null;

  const rows = [];
  for (const p of pairs) {
    const se = byTs.get(p.t.toFixed(3));
    const up = se ? se.t / 1000 - se.ts - upBase : null;  // 相对最小单程的额外上行排队时间
    const inject = se ? se.d / 1000 : null;
    const rest = (up != null) ? p.lag - up - inject : null;
    rows.push({ ts: p.t, lag: p.lag, up, inject, rest });
  }

  const avg = (k) => {
    const v = rows.map((r) => r[k]).filter((x) => x != null);
    return v.length ? v.reduce((a, b) => a + b, 0) / v.length : null;
  };
  console.log('\n=== 归因（逐条输入，与服务端按 ts 对齐）===');
  console.log(`匹配到服务端记录的输入: ${rows.filter((r) => r.inject != null).length} / ${rows.length}`);
  console.log(`上行走廊(相对最小) avg ${f(avg('up'))}ms | 注入 avg ${f(avg('inject'), 3)}ms | 其余(等帧+编码+下行+解码+绘制) avg ${f(avg('rest'))}ms`);

  const worst = rows.filter((r) => r.inject != null).sort((a, b) => b.lag - a.lag).slice(0, worstN);
  if (worst.length) {
    console.log(`\n最差的 ${worst.length} 条（t=客户端时刻 ms, lag=输入→上屏）:`);
    console.log('     t(ms)     lag     up  inject    rest');
    for (const r of worst) {
      console.log(`${f(r.ts, 1).padStart(10)}  ${f(r.lag).padStart(6)}  ${f(r.up).padStart(5)}  ${f(r.inject, 2).padStart(6)}  ${f(r.rest).padStart(6)}`);
    }
  }
} else if (pairs.length) {
  console.log('\n（只有客户端数据：无法拆分上行/注入，需要 gateway 的 /trace 一起导出）');
  const worst = pairs.slice().sort((a, b) => b.lag - a.lag).slice(0, worstN);
  console.log(`最差的 ${worst.length} 条输入→上屏延迟:`);
  for (const r of worst) console.log(`  t=${f(r.t, 1)}ms  lag=${f(r.lag)}ms`);
}

/* ── 光标可见延迟：直接比"画面里的光标坐标"和"手当时在哪" ──────────
 * 这是唯一能证明"鼠标卡"的指标。之前那套"输入 → 首个上屏帧"只是下界：
 * 它只证明"有新帧上屏"，不证明这一帧里的光标已经动了。
 * 这里用服务端随每帧记录的注入坐标（cx/cy，归一化），在客户端指针轨迹里
 * 找"手在哪一刻正好在这个位置"，差值就是这帧画面里光标"落后了多少毫秒"。
 * 全程只用客户端时钟 + 位置匹配，不依赖两边对钟。 */
const pmSamples = (c.events || [])
  .filter((e) => e.k === 'pm' && Number.isFinite(e.x) && Number.isFinite(e.y))
  .map((e) => ({ t: e.t, x: e.x, y: e.y }))
  .sort((a, b) => a.t - b.t);
const frmCursor = new Map();
for (const f of (s?.frames || [])) {
  if (Number.isFinite(f.cx) && f.cx >= 0 && f.pts != null) frmCursor.set(String(f.pts), f);
}
const renEvents = (c.events || []).filter((e) => e.k === 'ren' && e.pts != null);

if (pmSamples.length && frmCursor.size && renEvents.length) {
  /* 定义：一次"移动"= 指针从上一位置跳到新位置（位移 > MOVE）。
     指标 = 从「手到达新位置」到「该位置第一次出现在屏幕上的帧」之间的时间。
     注意不能反过来问"画面里这个坐标是多久前的手位置"：手在原地停住 700ms 再跳，
     那样算会得到 700ms 的假滞后（画面其实一直是准的）。 */
  const TOL = 0.015;   // 位置匹配容差（1920 宽下约 29px）
  const MOVE = 0.02;   // 认作"一次移动"的最小位移（占比）
  const lags = [];
  let moves = 0;
  let missed = 0;
  for (let i = 1; i < pmSamples.length; i++) {
    const a = pmSamples[i - 1];
    const b = pmSamples[i];
    const dx = Math.abs(b.x - a.x);
    const dy = Math.abs(b.y - a.y);
    if (dx + dy < MOVE) continue;
    moves++;
    /* 找 b.t 之后第一帧，其光标坐标 ≈ b 的位置 */
    let hit = null;
    for (const r of renEvents) {
      if (r.t < b.t) continue;
      if (r.t - b.t > 2000) break;
      const f = frmCursor.get(String(r.pts));
      if (!f) continue;
      if (Math.abs(f.cx - b.x) + Math.abs(f.cy - b.y) <= TOL) { hit = r; break; }
    }
    if (hit) lags.push(hit.t - b.t);
    else missed++;
  }
  lags.sort((a, b) => a - b);
  const q = (p) => +lags[Math.min(lags.length - 1, Math.floor((lags.length - 1) * p))].toFixed(1);
  console.log('\n=== 光标可见延迟（手到位 → 屏幕出现该位置）===');
  if (lags.length < 5) {
    console.log(`样本太少（${lags.length}/${moves}），请快速拖动鼠标 10 秒再录一段`);
  } else {
    console.log(`n=${lags.length}/${moves}  p50 ${q(0.5)}ms  p90 ${q(0.9)}ms  p99 ${q(0.99)}ms  max ${+lags[lags.length - 1].toFixed(1)}ms`);
    if (missed) console.log(`（${missed} 次移动在 2 秒内没在画面上出现，通常是丢帧/画面跟不上）`);
    console.log('（<50ms 基本跟手；>100ms 明显"拖手"；>200ms 难以使用）');
  }
} else {
  console.log('\n=== 光标可见延迟 ===');
  console.log('数据不足：需要客户端指针样本(pm) + 服务端带坐标的帧记录(frm.cx/cy)，请更新两端后再录');
}

/* ── 结论提示 ── */
console.log('\n=== 怎么读 ===');
console.log('· lag 持续 >150ms 且 up 很小  → 卡在「服务端出帧 + 下行 + 解码」这一段，看服务端出帧间隔与帧间隔>50ms 次数');
console.log('· up 明显偏大（>20ms）        → 卡在客户端→网关的控制流（QUIC 流排队）或网关转发');
console.log('· inject 偏大（>5ms）         → wwcap 主循环被业务拖住（抓帧/编码占满），查服务端日志的 cap/conv');
console.log('· lag 抖动大但平均值正常      → 掉帧/关键帧风暴，看 frameGapsOver50ms 与 server 的 key 帧比例');
