// app.js — Airi KV Cluster Dashboard 前端逻辑  v2
// 功能：
//   · 集群状态轮询（/admin/raft, 1.2s）+ KV 表格轮询（/admin/scan, 2s）
//   · GET / PUT / DELETE：可指定目标节点，PUT/DELETE 展示完整 307 转发链路
//   · 全节点读：同时向 5 个节点发 GET，对比一致性
//   · 批量写入：并发 20，实时进度 + 吞吐统计

const PORTS  = [8901, 8902, 8903, 8904, 8905];
const myPort = parseInt(location.port) || 8901;
const myId   = Math.max(0, PORTS.indexOf(myPort));

// 当前选择的目标节点（用于 GET/PUT/DELETE）
let targetNode = myId;

// 各节点最新 /admin/raft 快照（pollCluster 更新，供路由展示用）
const nodeInfo = Array(PORTS.length).fill(null);

// ── 初始化节点卡片 ──────────────────────────────────────────────────────────
function initCards() {
  const c = document.getElementById('cluster-cards');
  c.innerHTML = PORTS.map((p, i) =>
    '<div class="node-card" id="card-' + i + '" onclick="selectNode(' + i + ')" title="点击选为目标节点">' +
    '<div class="node-title">Node ' + i + ' <span>:' + p + '</span>' +
    (i === myId ? ' <span style="color:#d29922">[本]</span>' : '') +
    '</div>' +
    '<div class="node-state state-down" id="ns-' + i + '">\u25a1 连接中\u2026</div>' +
    '<div class="node-stats" id="nst-' + i + '">\u2014</div>' +
    '</div>'
  ).join('');
  document.getElementById('my-badge').textContent = 'Node ' + myId + ' \xb7 :' + myPort;
  buildNodeSel();
}

// ── 节点选择器 ────────────────────────────────────────────────────────────────
function buildNodeSel() {
  const el = document.getElementById('node-sel');
  if (!el) return;
  el.innerHTML = PORTS.map((p, i) =>
    '<button class="nsb' + (i === targetNode ? ' active' : '') + '" id="nsb-' + i + '" ' +
    'onclick="selectNode(' + i + ')">N' + i + '<br><span style="color:#484f58">:' + p + '</span></button>'
  ).join('');
}

function selectNode(i) {
  targetNode = i;
  document.querySelectorAll('.nsb').forEach((b, j) => {
    b.classList.toggle('active', j === i);
  });
}

// ── 轮询集群状态 ─────────────────────────────────────────────────────────────
async function pollCluster() {
  for (let i = 0; i < PORTS.length; i++) {
    try {
      const r = await fetch('http://127.0.0.1:' + PORTS[i] + '/admin/raft',
                            { signal: AbortSignal.timeout(900) });
      const d = await r.json();
      nodeInfo[i] = d;
      const card = document.getElementById('card-' + i);
      card.className = 'node-card ' +
        (d.state === 'Leader' ? 'leader' : d.state === 'Candidate' ? 'candidate' : '');
      const icons = { Leader: '\u26a1 LEADER', Follower: '\ud83d\udc65 Follower', Candidate: '\ud83d\uddf3\ufe0f Candidate' };
      const cls   = { Leader: 'state-leader', Follower: 'state-follower', Candidate: 'state-candidate' };
      const ns = document.getElementById('ns-' + i);
      ns.className   = 'node-state ' + (cls[d.state] || 'state-down');
      ns.textContent = (icons[d.state] || d.state);
      // 让贤状态徽标
      const xferBadge = (d.transferTarget !== undefined && d.transferTarget >= 0)
        ? ' &nbsp;<span style="color:#d29922;font-weight:bold">&rarr;N' + d.transferTarget + '</span>' : '';
      // 让贤统计（仅 Leader 且有记录时显示）
      const xferStats = (d.state === 'Leader' &&
                          d.transfersInitiated !== undefined && d.transfersInitiated > 0)
        ? '<br><span style="color:#d29922">transfers: <b>' +
          d.transfersSucceeded + '</b>/' + d.transfersInitiated + '</span>' : '';
      document.getElementById('nst-' + i).innerHTML =
        'term: <b>' + d.term + '</b> &nbsp; commit: <b>' + d.commitIndex +
        '</b> &nbsp; applied: <b>' + d.lastApplied + '</b><br>' +
        'kv: <b>' + d.kvSize + '</b> &nbsp; leader: <b>' +
        (d.leaderId < 0 ? '?' : 'Node ' + d.leaderId) + '</b>' + xferBadge + xferStats;
      // 同步 node-sel 按钮的 leader 样式
      const nsb = document.getElementById('nsb-' + i);
      if (nsb) {
        nsb.classList.toggle('nleader', d.state === 'Leader');
        nsb.classList.remove('ndown');
      }
    } catch {
      nodeInfo[i] = null;
      document.getElementById('card-' + i).className = 'node-card down';
      document.getElementById('ns-' + i).className   = 'node-state state-down';
      document.getElementById('ns-' + i).textContent = '\u274c 离线';
      document.getElementById('nst-' + i).textContent = '';
      const nsb = document.getElementById('nsb-' + i);
      if (nsb) { nsb.classList.add('ndown'); nsb.classList.remove('nleader','active'); }
    }
  }
}

// ── 轮询 KV 表格 ─────────────────────────────────────────────────────────────
async function pollKv() {
  try {
    const t = Date.now();
    const r = await fetch('http://127.0.0.1:' + PORTS[myId] + '/admin/scan',
                          { signal: AbortSignal.timeout(900) });
    const d = await r.json();
    document.getElementById('kv-count').textContent  = '(' + d.count + ' \u6761)';
    document.getElementById('scan-time').textContent = (Date.now() - t) + 'ms';
    const tb = document.getElementById('kv-body');
    if (!d.pairs.length) {
      tb.innerHTML = '<tr><td colspan="2" style="color:#484f58;text-align:center">(\u7a7a)</td></tr>';
      return;
    }
    tb.innerHTML = d.pairs.map(p =>
      '<tr><td title="' + esc(p.key) + '">'   + esc(p.key) +
      '</td><td title="' + esc(p.value) + '">' + esc(p.value) + '</td></tr>'
    ).join('');
  } catch {
    document.getElementById('kv-body').innerHTML =
      '<tr><td colspan="2" style="color:#ff7b72">\u83b7\u53d6\u5931\u8d25</td></tr>';
  }
}

// ── 工具函数 ─────────────────────────────────────────────────────────────────
function esc(s) {
  return String(s)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;');
}

function setResp(text, ok, subtitle) {
  const el = document.getElementById('response');
  el.textContent = text;
  el.style.color = ok ? '#a5d6ff' : '#ff7b72';
  el.classList.remove('flash');
  void el.offsetWidth;
  el.classList.add('flash');
  const sub = document.getElementById('resp-subtitle');
  if (sub && subtitle !== undefined) sub.textContent = subtitle;
}

// ── 构建 HTTP 路由链路描述 ────────────────────────────────────────────────────
// srcInfo: /admin/raft 快照（或 null 表示离线/超时）
// 返回多行字符串，在 Raft 帧之前展示
function buildRouteChain(nodeIdx, srcInfo, method, key) {
  const lines = [];
  const port  = PORTS[nodeIdx];
  const url   = 'http://127.0.0.1:' + port + '/kv/' + key;

  lines.push('─── HTTP 请求路由 ─────────────────────────────────────────');
  if (!srcInfo) {
    lines.push('① Client  →  Node ' + nodeIdx + '  (:' + port + ')  [状态未知]');
    lines.push('   ' + method + '  ' + url);
  } else if (srcInfo.state === 'Leader') {
    lines.push('① Client  →  Node ' + nodeIdx + '  (⚡ Leader, term=' + srcInfo.term + ')');
    lines.push('   ' + method + '  ' + url);
    lines.push('   (当前节点即 Leader，直接处理，无需转发)');
  } else {
    const leaderId   = srcInfo.leaderId;
    const leaderPort = leaderId >= 0 ? PORTS[leaderId] : '?';
    const leaderUrl  = leaderId >= 0
      ? 'http://127.0.0.1:' + leaderPort + '/kv/' + key
      : '(leader 未知)';
    lines.push('① Client  →  Node ' + nodeIdx +
               '  (' + srcInfo.state + ', term=' + srcInfo.term + ')');
    lines.push('   ' + method + '  ' + url);
    lines.push('');
    lines.push('② Node ' + nodeIdx + '  →  307 Temporary Redirect');
    lines.push('   Location: ' + leaderUrl);
    lines.push('   Body:  {"fromNode":' + nodeIdx +
               ', "fromState":"' + srcInfo.state +
               '", "leader":' + leaderId +
               ', "leaderUrl":"' + leaderUrl + '", ...}');
    lines.push('');
    lines.push('③ fetch 自动跟随  →  Node ' + leaderId + '  (Leader, :' + leaderPort + ')');
    lines.push('   ' + method + '  ' + leaderUrl);
  }
  lines.push('');
  lines.push('─── Raft 共识过程 ─────────────────────────────────────────');
  return lines.join('\n');
}

// ── 渲染单个 Raft 响应帧 ─────────────────────────────────────────────────────
function renderRaftFrame(obj, prevText) {
  const lines = [];
  const tag = (s) => '[' + s + ']';
  if (obj.status === 'accepted') {
    lines.push('\u23f3 ' + (obj.phase || 'leader 已接受请求'));
    lines.push('');
    lines.push('  ' + tag('Leader')      + '     Node ' + obj.leader + '  (term=' + obj.term + ')');
    lines.push('  ' + tag('Cluster')     + '    ' + obj.clusterSize + ' 节点，quorum=' + obj.quorum);
    lines.push('  ' + tag('commitIndex') + ' before=' + obj.commitIndexBefore);
    lines.push('  ' + tag('lastLogIndex')+ ' before=' + obj.lastLogIndexBefore);
    lines.push('  ' + tag('cmd')         + '        ' + obj.cmd);
    if (obj.timeline && obj.timeline.length) {
      lines.push('');
      lines.push('  时间线（正在执行）：');
      obj.timeline.forEach((s, i) => lines.push('    ' + (i + 1) + ') ' + s));
    }
    return lines.join('\n');
  }
  if (obj.status === 'applied' && obj.ok) {
    const head = prevText ? prevText.split('\n').slice(0, 8).join('\n') + '\n\n' : '';
    lines.push('\u2705 ' + (obj.phase || '已 apply'));
    lines.push('');
    lines.push('  ' + tag('Leader')       + '         Node ' + obj.leader + '  (term=' + obj.term + ')');
    lines.push('  ' + tag('logIndex')     + '       ' + obj.logIndex + '  ← 本次写入全局日志下标');
    lines.push('  ' + tag('commitIndex')  + '    ' + obj.commitIndexBefore + ' → ' + obj.commitIndexAfter);
    lines.push('  ' + tag('lastApplied')  + '    ' + obj.lastApplied);
    lines.push('  ' + tag('quorum')       + '         ' + obj.quorum + '/' + obj.clusterSize + ' 多数派已确认');
    lines.push('  ' + tag('kvSize')       + '         ' + obj.kvSize);
    lines.push('  ' + tag('端到端耗时')   + '   ' + obj.latencyMs.toFixed(2) + ' ms');
    if (obj.explain) {
      lines.push('');
      lines.push('  ' + obj.explain);
    }
    return head + lines.join('\n');
  }
  if (obj.status === 'applied' && !obj.ok) {
    lines.push('\u274c ' + (obj.phase || 'apply 失败'));
    lines.push('  leader=Node ' + obj.leader + '  term=' + obj.term +
               '  耗时=' + (obj.latencyMs || 0).toFixed(2) + 'ms');
    if (obj.error) lines.push('  原因：' + obj.error);
    return lines.join('\n');
  }
  if (obj.status === 'redirect') {
    lines.push('\u21aa\ufe0f  307 重定向到 Leader');
    lines.push('  ' + tag('fromNode')  + '  Node ' + obj.fromNode + ' (' + obj.fromState + ', term=' + obj.term + ')');
    lines.push('  ' + tag('leader')    + '    Node ' + obj.leader);
    lines.push('  ' + tag('leaderUrl') + ' ' + obj.leaderUrl);
    if (obj.hint) lines.push('  ' + obj.hint);
    return lines.join('\n');
  }
  if (obj.status === 'no_leader') {
    lines.push('\u26a0\ufe0f  集群当前无 Leader');
    lines.push('  ' + tag('fromNode') + ' Node ' + obj.fromNode + ' (' + obj.fromState + ', term=' + obj.term + ')');
    lines.push('  ' + (obj.reason || ''));
    if (obj.hint) lines.push('  ' + obj.hint);
    return lines.join('\n');
  }
  return JSON.stringify(obj, null, 2);
}

// ── 操作日志 ─────────────────────────────────────────────────────────────────
function addLog(op, key, extra, status, ms) {
  const le = document.getElementById('log-entries');
  if (le.querySelector('span')) le.innerHTML = '';
  const now    = new Date().toTimeString().slice(0, 8);
  const okCls  = status < 400 ? 'ok' : 'err';
  const valPart = extra
    ? ' = <span style="color:#8b949e">"' +
      esc(extra.length > 22 ? extra.slice(0, 22) + '\u2026' : extra) + '"</span>'
    : '';
  const d = document.createElement('div');
  d.className = 'log-entry flash';
  d.innerHTML =
    '<span class="ts">' + now + '</span>' +
    '<span class="op-' + op + '">' + op + '</span>  ' +
    '<b>' + esc(key) + '</b>' + valPart +
    '  <span class="' + okCls + '">[' + status + ']</span>' +
    '<span class="ms">' + ms + 'ms</span>';
  le.prepend(d);
  if (le.children.length > 80) le.lastChild.remove();
}

// ── 内部：读取目标节点状态 ───────────────────────────────────────────────────
async function fetchNodeState(nodeIdx) {
  try {
    const r = await fetch('http://127.0.0.1:' + PORTS[nodeIdx] + '/admin/raft',
                          { signal: AbortSignal.timeout(600) });
    return await r.json();
  } catch { return null; }
}

// ── 内部：完整写操作（支持 chunked stream + 路由链路展示）────────────────────
async function doWrite(method) {
  const key = document.getElementById('key-in').value.trim();
  const val = document.getElementById('val-in').value;
  if (!key) { setResp('请输入 Key', false, ''); return; }

  const t    = Date.now();
  const info = nodeInfo[targetNode] || await fetchNodeState(targetNode);
  const chain = buildRouteChain(targetNode, info, method, key);

  setResp(chain + '\n\u23f3 提交中…', true,
          'Node ' + targetNode + (info ? ' (' + info.state + ')' : ''));

  const url = 'http://127.0.0.1:' + PORTS[targetNode] + '/kv/' + encodeURIComponent(key);
  try {
    const r = await fetch(url, {
      method,
      body: method === 'DELETE' ? undefined : val,
      redirect: 'follow'
    });
    if (!r.ok || !r.body) {
      const d = await r.json().catch(() => ({}));
      const txt = chain + '\n' + renderRaftFrame(d, '');
      setResp(txt, false, 'Node ' + targetNode + ' → 错误');
      addLog(method === 'DELETE' ? 'DEL' : 'PUT', key, val, r.status, Date.now() - t);
      pollKv();
      return;
    }
    // 读 NDJSON chunked stream
    const reader  = r.body.getReader();
    const decoder = new TextDecoder();
    let buf  = '';
    let last = null;
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      buf += decoder.decode(value, { stream: true });
      const lines = buf.split('\n');
      buf = lines.pop() || '';
      for (const line of lines) {
        const s = line.trim();
        if (!s) continue;
        try {
          const obj = JSON.parse(s);
          last = obj;
          const raftText = renderRaftFrame(obj,
            document.getElementById('response').textContent.split('─── Raft')[1] || '');
          const fullText = chain + '\n' + raftText;
          setResp(fullText, obj.status === 'applied' ? (obj.ok !== false) : true,
                  'Node ' + targetNode + ' → Node ' +
                  (obj.leader !== undefined ? obj.leader : '?') + ' (Leader)');
        } catch { /* skip non-JSON */ }
      }
    }
    const op = method === 'DELETE' ? 'DEL' : 'PUT';
    if (last) addLog(op, key, val, last.ok ? 200 : 500, Date.now() - t);
  } catch (e) { setResp(chain + '\n网络错误: ' + e.message, false, ''); }
  pollKv();
}

// ── KV 操作：GET ─────────────────────────────────────────────────────────────
async function doGet() {
  const key = document.getElementById('key-in').value.trim();
  if (!key) { setResp('请输入 Key', false, ''); return; }
  const t    = Date.now();
  const port = PORTS[targetNode];
  const url  = 'http://127.0.0.1:' + port + '/kv/' + encodeURIComponent(key);
  try {
    const r = await fetch(url, { signal: AbortSignal.timeout(1200) });
    const d = await r.json();
    // 构建 GET 展示（本地读，展示节点元数据）
    const lines = [];
    lines.push('─── GET 请求 ──────────────────────────────────────────────');
    lines.push('  URL:        ' + url);
    lines.push('');
    lines.push('─── 响应 ──────────────────────────────────────────────────');
    if (d.ok) {
      lines.push('  \u2705 找到');
      lines.push('  key:          ' + d.key);
      lines.push('  value:        ' + d.value);
    } else {
      lines.push('  \u274c 未找到  (' + (d.error || 'not found') + ')');
      lines.push('  key:          ' + d.key);
    }
    lines.push('');
    lines.push('─── 服务节点元数据 ────────────────────────────────────────');
    const srv     = d.servedBy !== undefined ? d.servedBy : targetNode;
    const srvState= d.servedByState || '?';
    lines.push('  servedBy:     Node ' + srv + ' (' + srvState + ', term=' + (d.term||'?') + ')');
    lines.push('  lastApplied:  ' + (d.lastApplied !== undefined ? d.lastApplied : '?'));
    if (srvState !== 'Leader') {
      lines.push('');
      lines.push('  \u26a0\ufe0f  Follower 本地读 — 若落后 Leader，数据可能稍旧');
      lines.push('     （Raft 默认读不走 quorum，可能看到 stale 值）');
    }
    setResp(lines.join('\n'), r.ok, 'Node ' + srv + ' (' + srvState + ')');
    addLog('GET', key, r.ok ? d.value : '', r.status, Date.now() - t);
  } catch (e) { setResp('网络错误: ' + e.message, false, ''); }
  pollKv();
}

async function doPut()  { await doWrite('PUT');    }
async function doDel()  { await doWrite('DELETE'); }

// ── 全节点读：同时向 5 个节点发 GET，对比一致性 ───────────────────────────
async function doReadAll() {
  const key = document.getElementById('key-in').value.trim();
  if (!key) { setResp('请先输入 Key', false, ''); return; }
  setResp('正在查询所有节点…', true, '全节点一致性检查');
  const results = await Promise.all(PORTS.map(async (port, i) => {
    try {
      const r = await fetch('http://127.0.0.1:' + port + '/kv/' + encodeURIComponent(key),
                            { signal: AbortSignal.timeout(1000) });
      const d = await r.json();
      return { node: i, port, found: r.ok, value: d.value,
               state: d.servedByState || '?', term: d.term, applied: d.lastApplied };
    } catch { return { node: i, port, offline: true }; }
  }));

  const lines = [];
  lines.push('─── 全节点读取  key="' + key + '" ─────────────────────────');
  lines.push('');
  const values = new Set();
  for (const r of results) {
    if (r.offline) {
      lines.push('  Node ' + r.node + '  (:' + r.port + ')   \u274c 离线');
      continue;
    }
    const icon    = r.state === 'Leader' ? '\u26a1 Leader  ' : '\ud83d\udc65 Follower';
    const valText = r.found ? '"' + r.value + '"' : '(not found)';
    lines.push('  Node ' + r.node + '  ' + icon +
               '  term=' + (r.term ?? '?') +
               '  applied=' + (r.applied ?? '?') +
               '  →  ' + valText);
    if (r.found) values.add(r.value);
  }
  lines.push('');
  lines.push('─── 一致性结论 ────────────────────────────────────────────');
  if (values.size === 0) {
    lines.push('  所有在线节点均未找到该 key');
  } else if (values.size === 1) {
    lines.push('  \u2705 数据一致：所有在线节点返回相同值');
    lines.push('  value = "' + [...values][0] + '"');
  } else {
    lines.push('  \u26a0\ufe0f 数据不一致！检测到 ' + values.size + ' 种不同值：');
    [...values].forEach((v, i) => lines.push('    ' + (i + 1) + ') "' + v + '"'));
    lines.push('  （Follower 可能落后 Leader，等下次 apply 后一致）');
  }
  setResp(lines.join('\n'), values.size <= 1, '全节点读  key="' + key + '"');
  addLog('GET', key, '', values.size === 1 ? 200 : 206, 0);
}

// ── 批量写入 ─────────────────────────────────────────────────────────────────
let batchActive = false;
let batchKeyOffset = 0;  // 跨批次单调递增，确保每次批量写入的 key 不重复

function batchPreset(n) {
  document.getElementById('batch-n').value = n;
  startBatch();
}

async function startBatch() {
  if (batchActive) return;
  const n      = Math.min(5000, Math.max(1, parseInt(document.getElementById('batch-n').value) || 100));
  const prefix = (document.getElementById('batch-prefix').value || 'bench').trim();
  batchActive  = true;
  document.getElementById('btn-batch-start').disabled = true;
  document.getElementById('btn-batch-stop').disabled  = false;

  // 找到 Leader 节点直接发（跳过 307 重定向，提高吞吐）
  let leaderIdx = myId;
  for (let i = 0; i < PORTS.length; i++) {
    if (nodeInfo[i] && nodeInfo[i].state === 'Leader') { leaderIdx = i; break; }
  }
  const leaderPort = PORTS[leaderIdx];

  let success = 0, failed = 0;
  const t0 = Date.now();

  // 生成所有任务（key 从 batchKeyOffset 起，确保跨批次不重复）
  const startIdx = batchKeyOffset;
  const tasks = [];
  for (let i = 0; i < n; i++) {
    const k = prefix + ':' + String(startIdx + i).padStart(6, '0');
    const v = 'v' + Math.random().toString(36).slice(2, 10);
    tasks.push({ k, v });
  }

  // 并发 20 控制
  const CONCURRENT = 20;
  let idx = 0;

  function updateBatchUI() {
    const done    = success + failed;
    const pct     = n > 0 ? (done / n * 100).toFixed(1) : 0;
    const elapsed = Date.now() - t0;
    const ops     = elapsed > 0 ? (done / elapsed * 1000).toFixed(0) : 0;
    document.getElementById('pbfill').style.width = pct + '%';
    document.getElementById('batch-status').textContent =
      done + '/' + n + '  \u2713' + success + ' \u2715' + failed +
      '  |  ' + ops + ' ops/s  ' + elapsed + 'ms';
  }

  async function worker() {
    while (idx < tasks.length && batchActive) {
      const { k, v } = tasks[idx++];
      try {
        const r = await fetch('http://127.0.0.1:' + leaderPort + '/kv/' + encodeURIComponent(k),
                              { method: 'PUT', body: v, redirect: 'follow' });
        // 消耗 stream 以避免连接积压
        if (r.body) await r.body.cancel();
        success++;
      } catch { failed++; }
      updateBatchUI();
    }
  }

  const workers = [];
  for (let w = 0; w < CONCURRENT; w++) workers.push(worker());
  await Promise.all(workers);

  const elapsed = Date.now() - t0;
  const ops     = elapsed > 0 ? (n / elapsed * 1000).toFixed(0) : 0;
  const finalMsg = batchActive
    ? '\u2705 完成: ' + n + ' 条  \u2713' + success + ' \u2715' + failed +
      '  |  ' + ops + ' ops/s  ' + elapsed + 'ms'
    : '\u23f9 已中止  \u2713' + success + ' \u2715' + failed +
      '  |  ' + ops + ' ops/s  ' + elapsed + 'ms';
  document.getElementById('batch-status').textContent = finalMsg;
  document.getElementById('pbfill').style.width =
    batchActive ? '100%' : (((success + failed) / n) * 100).toFixed(1) + '%';

  addLog('BATCH', prefix + ':*', String(n), success > 0 ? 200 : 500, elapsed);
  setResp(
    '批量写入完成\n\n' +
    '  前缀:     ' + prefix + '\n' +
    '  总数:     ' + n + '\n' +
    '  成功:     ' + success + '\n' +
    '  失败:     ' + failed + '\n' +
    '  耗时:     ' + elapsed + ' ms\n' +
    '  吞吐:     ' + ops + ' ops/s\n' +
    '  目标节点: Node ' + leaderIdx + ' (Leader, :' + leaderPort + ')\n' +
    '  并发度:   ' + CONCURRENT,
    failed === 0,
    '批量写入  ' + n + ' 条'
  );

  batchKeyOffset += success;  // 下次批量写入从此处续接，保证 key 唯一
  batchActive = false;
  document.getElementById('btn-batch-start').disabled = false;
  document.getElementById('btn-batch-stop').disabled  = true;
  pollKv();
}

function stopBatch() {
  batchActive = false;
}

// ── Leader Transfer ─────────────────────────────────────────────────────────
async function doTransfer(targetId) {
  // 找到当前 Leader
  let leaderIdx = -1;
  for (let i = 0; i < PORTS.length; i++) {
    if (nodeInfo[i] && nodeInfo[i].state === 'Leader') { leaderIdx = i; break; }
  }
  if (leaderIdx < 0) {
    setResp('\u26a0\ufe0f 当前无 Leader，请稍等集群选主后再试', false, 'Leader Transfer');
    return;
  }
  const info = nodeInfo[leaderIdx];
  const url  = 'http://127.0.0.1:' + PORTS[leaderIdx] + '/admin/transfer' +
               (targetId >= 0 ? '?to=' + targetId : '');
  try {
    const r = await fetch(url, { method: 'POST', signal: AbortSignal.timeout(1500) });
    const d = await r.json().catch(() => ({}));
    const tgtStr = targetId >= 0 ? 'Node ' + targetId : '\u81ea\u52a8\uff08matchIndex \u6700\u9ad8\uff09';
    const lines = [
      '\u26a1 Leader Transfer \u5df2\u89e6\u53d1',
      '',
      '  \u6e90\u8282\u70b9:   Node ' + leaderIdx + '  (term=' + (info?.term ?? '?') + ')',
      '  \u76ee\u6807\u8282\u70b9: ' + tgtStr,
      '  \u72b6\u6001:     ' + (d.ok ? '\u2705 \u542f\u52a8\u6210\u529f' : '\u274c ' + (d.error || '\u5931\u8d25')),
      '',
      '  \u8bf7\u89c2\u5bdf\u8282\u70b9\u5361\u7247 Leader \u53d8\u5316\uff08\u7ea6 50\u2013200\u00a0ms\uff09',
      '  \u8ba9\u8d42\u671f\u95f4\u5199\u5165\u88ab\u6682\u505c\uff0c\u5b8c\u6210\u540e\u81ea\u52a8\u6062\u590d'
    ];
    setResp(lines.join('\n'), d.ok, 'Leader Transfer');
  } catch (e) { setResp('\u7f51\u7edc\u9519\u8bef: ' + e.message, false, 'Leader Transfer'); }
  // 立刻刷新状态卡片
  setTimeout(pollCluster, 150);
  setTimeout(pollCluster, 400);
  setTimeout(pollCluster, 900);
}

// ── 启动 ─────────────────────────────────────────────────────────────────────
initCards();
pollCluster();
pollKv();
setInterval(pollCluster, 1200);
setInterval(pollKv, 2000);
