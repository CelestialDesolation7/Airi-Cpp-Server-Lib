#!/usr/bin/env bash
# scripts/bench_raft.sh — 分布式 KV 集群压测 + Leader CPU profile
#
# 目的：
#   /health 压测只能反映 HTTP 框架自身的开销，无法暴露分布式系统的真实热点。
#   本脚本启动 5 节点 Raft 集群，对 Leader 注入混合读写负载，并用 macOS
#   Instruments (xctrace) 在 Leader 节点上抓 CPU profile，得到包含
#   AppendEntries 广播 / 日志持久化 / kqueue 事件分发 / RocksDB 写入 / apply
#   回调的完整火焰图。
#
# 用法：
#   bash scripts/bench_raft.sh                    # 默认 30s wrk，含 profile
#   bash scripts/bench_raft.sh --duration 60      # 自定义压测时长
#   bash scripts/bench_raft.sh --no-profile       # 跳过 xctrace 采样
#   bash scripts/bench_raft.sh --chaos            # 中途 kill 一个 follower
#
# 产物：
#   benchmark/day37/raft/
#     ├── cluster.log                              五个节点的合并日志
#     ├── wrk_put.txt / wrk_get.txt                wrk 输出（吞吐 + 分位延迟）
#     ├── leader.trace/                            Instruments trace（双击打开）
#     ├── leader_top.txt                           导出的 top 函数表
#     └── raft_state_final.json                    最终 5 节点 /admin/raft 快照
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

DURATION=30
DO_PROFILE=1
DO_CHAOS=0
BIN="${BIN:-build-profile/examples/kv_node}"   # RelWithDebInfo 版本（有符号）
OUT="benchmark/day37/raft"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --duration)    DURATION="$2"; shift 2;;
        --no-profile)  DO_PROFILE=0; shift;;
        --chaos)       DO_CHAOS=1; shift;;
        *) echo "unknown arg: $1"; exit 1;;
    esac
done

[[ -x "$BIN" ]] || { echo "错误: 未找到 $BIN — 请先 cmake --build build-profile --target kv_node"; exit 1; }
command -v wrk >/dev/null || { echo "错误: 请先 brew install wrk"; exit 1; }

mkdir -p "$OUT"
rm -rf "$OUT"/leader.trace "$OUT"/*.txt "$OUT"/*.log "$OUT"/*.json 2>/dev/null || true
rm -rf ./kv_raft_state                                   # 清空持久化数据保证可复现
mkdir -p /tmp/airi_bench
LOG="$OUT/cluster.log"; : > "$LOG"

echo "═══════════════════════════════════════════════════════════════"
echo "  Airi 分布式 KV 压测  —  5 nodes / 30s wrk / xctrace profile"
echo "═══════════════════════════════════════════════════════════════"

# ── 启动 5 节点集群 ─────────────────────────────────────────────────────────
echo "[1/5] 启动 5 节点 Raft 集群..."
PIDS=()
for i in 0 1 2 3 4; do
    "$BIN" --id $i --nodes 5 --persist --static-dir examples/static/kv \
        >> "$LOG" 2>&1 &
    PIDS[$i]=$!
done
cleanup() {
    echo
    echo "清理：终止所有节点..."
    for p in "${PIDS[@]}"; do kill -TERM "$p" 2>/dev/null || true; done
    wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# ── 等待 Leader 选出 ────────────────────────────────────────────────────────
echo "[2/5] 等待 Leader 选举..."
LEADER_PORT=""
for _ in $(seq 1 30); do
    sleep 0.5
    for port in 8901 8902 8903 8904 8905; do
        STATE=$(curl -sf "http://127.0.0.1:$port/admin/raft" 2>/dev/null \
                | python3 -c "import sys,json;print(json.load(sys.stdin).get('state',''))" 2>/dev/null || true)
        if [[ "$STATE" == "Leader" ]]; then
            LEADER_PORT=$port
            break 2
        fi
    done
done
[[ -n "$LEADER_PORT" ]] || { echo "Leader 未在 15s 内选出"; exit 1; }
LEADER_ID=$(curl -s "http://127.0.0.1:$LEADER_PORT/admin/raft" \
            | python3 -c "import sys,json;print(json.load(sys.stdin)['id'])")
LEADER_PID=${PIDS[$LEADER_ID]}
echo "    Leader = Node $LEADER_ID  (HTTP :$LEADER_PORT  PID=$LEADER_PID)"

# ── 预热：写 200 个 key，让 RocksDB / log 进入稳态 ───────────────────────
echo "[3/5] 预热 (200 个 PUT)..."
for i in $(seq 1 200); do
    curl -s -X PUT "http://127.0.0.1:$LEADER_PORT/kv/warm_$i" -d "v$i" > /dev/null
done
echo "    KV size = $(curl -s "http://127.0.0.1:$LEADER_PORT/admin/raft" \
                     | python3 -c "import sys,json;print(json.load(sys.stdin)['kvSize'])")"

# ── wrk Lua 脚本：50% GET + 50% PUT ────────────────────────────────────────
cat > /tmp/airi_bench/mix.lua <<'EOF'
math.randomseed(os.time())
local counter = 0
request = function()
    counter = counter + 1
    local k = "k" .. (counter % 1000)
    if counter % 2 == 0 then
        wrk.headers["Content-Type"] = "text/plain"
        return wrk.format("PUT", "/kv/" .. k, nil, "val_" .. counter)
    else
        return wrk.format("GET", "/kv/" .. k)
    end
end
EOF

# ── 启动 wrk + 同步 xctrace 采样 Leader ─────────────────────────────────────
echo "[4/5] 压测 ${DURATION}s  (50% GET / 50% PUT mixed)..."
echo "      wrk -t4 -c50 -d${DURATION}s --latency http://127.0.0.1:$LEADER_PORT/"
wrk -t4 -c50 -d${DURATION}s --latency -s /tmp/airi_bench/mix.lua \
    "http://127.0.0.1:$LEADER_PORT/" > "$OUT/wrk_mixed.txt" 2>&1 &
WRK_PID=$!

if [[ $DO_PROFILE -eq 1 ]]; then
    sleep 3
    PROFILE_SECS=$(( DURATION - 6 ))
    [[ $PROFILE_SECS -lt 8 ]] && PROFILE_SECS=8
    echo "      xctrace recording ${PROFILE_SECS}s on PID $LEADER_PID..."
    /usr/bin/xctrace record --template "CPU Profiler" --attach "$LEADER_PID" \
        --time-limit "${PROFILE_SECS}s" \
        --output "$OUT/leader.trace" 2>&1 | tail -3
fi

# 可选 chaos：中途 kill 一个 follower
if [[ $DO_CHAOS -eq 1 ]]; then
    sleep $(( DURATION / 2 ))
    VICTIM=$(( (LEADER_ID + 1) % 5 ))
    echo "      [chaos] kill Node $VICTIM"
    kill -9 "${PIDS[$VICTIM]}" 2>/dev/null || true
fi

wait "$WRK_PID" || true
echo
set +e   # 后续步骤允许单点失败，不要被 trap 提前拍死

# ── 导出 top 函数（穷人版火焰图）────────────────────────────────────────────
if [[ $DO_PROFILE -eq 1 && -d "$OUT/leader.trace" ]]; then
    echo "[5/5] 导出 top 函数..."
    /usr/bin/xctrace export --input "$OUT/leader.trace" \
        --xpath '//trace-toc/run/data/table[@schema="cpu-profile"]' \
        > /tmp/airi_bench/profile.xml 2>&1 || true
    grep -oE '<frame [^>]*name="[^"]+"' /tmp/airi_bench/profile.xml \
        | sed -E 's/.*name="([^"]+)".*/\1/' \
        | sort | uniq -c | sort -rn | head -50 > "$OUT/leader_top.txt"
fi

# ── 最终集群状态 ────────────────────────────────────────────────────────────
echo "[ok] 收集最终 Raft 状态..."
{
    echo "["
    for port in 8901 8902 8903 8904 8905; do
        curl -sf "http://127.0.0.1:$port/admin/raft" 2>/dev/null || echo "{\"port\":$port,\"down\":true}"
        [[ $port -lt 8905 ]] && echo ","
    done
    echo "]"
} > "$OUT/raft_state_final.json"

echo
echo "═══════════════════════════════════════════════════════════════"
echo "  完成！产物位于 $OUT/"
ls -la "$OUT/"
echo
echo "  wrk 结果摘要："
grep -E "Requests/sec|Latency.*Distribution|^\s*(50|75|90|99)%" "$OUT/wrk_mixed.txt" || true
[[ -f "$OUT/leader_top.txt" ]] && {
    echo
    echo "  Leader CPU profile top 15:"
    head -15 "$OUT/leader_top.txt" | awk '{printf "    %6d  %s\n",$1,$2}'
}
echo
echo "  在 Finder 中双击 $OUT/leader.trace 用 Instruments 查看火焰图"
echo "═══════════════════════════════════════════════════════════════"
