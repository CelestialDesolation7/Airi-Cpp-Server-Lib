#!/usr/bin/env bash
# leader_transfer_demo.sh
# ─────────────────────────────────────────────────────────────────────────────
# 对比"被动选主"与"主动让贤"在 Leader 故障/下线场景的写空窗时长。
#
# 用法：
#   bash scripts/leader_transfer_demo.sh kill9     # 场景 A：暴力 kill -9
#   bash scripts/leader_transfer_demo.sh sigterm   # 场景 B：SIGTERM 触发优雅让贤
#   bash scripts/leader_transfer_demo.sh transfer  # 场景 C：HTTP 主动 transfer
#
# 输出：
#   /tmp/leader_transfer_<scenario>.csv   →  ts_ms,latency_ms,http_code
#   /tmp/leader_transfer_<scenario>.summary.txt  →  统计摘要
# ─────────────────────────────────────────────────────────────────────────────
set -u
SCEN="${1:-kill9}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/examples/kv_node"
[ -x "$BIN" ] || { echo "kv_node 未构建，请先 cmake --build build --target kv_node"; exit 1; }

OUT_DIR=/tmp
CSV="$OUT_DIR/leader_transfer_$SCEN.csv"
SUM="$OUT_DIR/leader_transfer_$SCEN.summary.txt"
LOG_DIR="$OUT_DIR/kv_node_logs"
mkdir -p "$LOG_DIR"
rm -f "$CSV" "$SUM"

# ── 启动 3 节点集群（错峰启动，避免 term=1 死锁）───────────────────────────
PIDS=()
for i in 0 1 2; do
    "$BIN" --id $i --nodes 3 >"$LOG_DIR/node$i.log" 2>&1 &
    PIDS+=($!)
    sleep 0.3
done
trap 'for p in "${PIDS[@]}"; do kill -9 $p 2>/dev/null; done' EXIT

echo "等待集群选主..."
sleep 3

# 找出当前 Leader（HTTP 端口 8901/8902/8903）
LEADER_ID=-1
LEADER_PORT=0
LEADER_PID=0
for attempt in $(seq 1 30); do
    for i in 0 1 2; do
        PORT=$((8901 + i))
        STATE=$(curl -s --max-time 1 "http://127.0.0.1:$PORT/admin/raft" 2>/dev/null | python3 -c 'import sys,json;d=json.load(sys.stdin);print(d.get("state",""))' 2>/dev/null)
        if [ "$STATE" = "Leader" ]; then
            LEADER_ID=$i
            LEADER_PORT=$PORT
            LEADER_PID=${PIDS[$i]}
            break 2
        fi
    done
    sleep 1
done
if [ "$LEADER_ID" -lt 0 ]; then
    echo "未发现 Leader，集群可能未起来。检查 $LOG_DIR/"
    exit 1
fi
echo "Leader = Node $LEADER_ID  (port $LEADER_PORT  pid $LEADER_PID)"

# 客户端入口：选一个非 Leader 节点（靠 -L 跟随 307 重定向到当前 Leader）。
# 这样 Leader 进程被 kill 后，客户端 TCP 仍能连上 follower → follower 把请求重定向到新 leader。
ENTRY_PORT=0
for i in 0 1 2; do
    P=$((8901 + i))
    [ "$P" != "$LEADER_PORT" ] && ENTRY_PORT=$P && break
done
echo "客户端入口 (follower 节点) = port $ENTRY_PORT"

# ── 后台压测：每 5ms PUT 一次到 follower 入口 ───────────────────────────────
echo "开始 PUT 压测，输出到 $CSV ..."
START_NS=$(python3 -c 'import time;print(int(time.time()*1000))')
(
  N=0
  END=$((START_NS + 15000))   # 跑 15 秒
  while :; do
      NOW=$(python3 -c 'import time;print(int(time.time()*1000))')
      [ "$NOW" -ge "$END" ] && break
      N=$((N+1))
      OUT=$(curl -s -L -o /dev/null --max-time 1 \
            -w "%{time_total},%{http_code}" \
            -X PUT "http://127.0.0.1:$ENTRY_PORT/kv/k$N" -d "v$N" 2>/dev/null)
      LAT_S="${OUT%,*}"
      CODE="${OUT##*,}"
      [ -z "$LAT_S" ] && LAT_S=1.0
      [ -z "$CODE" ] && CODE=000
      LAT_MS=$(python3 -c "print(int(float('$LAT_S')*1000))" 2>/dev/null || echo 1000)
      TS=$((NOW - START_NS))
      echo "$TS,$LAT_MS,$CODE" >>"$CSV"
      # ~5ms 节奏（curl 已自带耗时；不再额外 sleep 以提高分辨率）
  done
) &
LOAD_PID=$!

# ── 5 秒后注入故障 ──────────────────────────────────────────────────────────
sleep 5
INJECT_TS=$(($(python3 -c 'import time;print(int(time.time()*1000))') - START_NS))
echo "[$INJECT_TS ms] 注入故障：场景=$SCEN"
case "$SCEN" in
    kill9)
        kill -9 $LEADER_PID
        ;;
    sigterm)
        kill -TERM $LEADER_PID
        ;;
    transfer)
        # 通过 HTTP 触发主动让贤（Leader 节点继续在线）
        curl -s -X POST "http://127.0.0.1:$LEADER_PORT/admin/transfer" >/dev/null
        ;;
    *)
        echo "未知场景: $SCEN"; exit 1;;
esac

# ── 等剩余压测结束 ───────────────────────────────────────────────────────────
wait $LOAD_PID 2>/dev/null

# ── 生成摘要：写空窗 = 连续非 200 的最长持续时间 ────────────────────────────
python3 - "$CSV" "$SUM" "$INJECT_TS" "$SCEN" <<'PY'
import sys, csv
csv_path, sum_path, inject_ts, scen = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
rows=[]
with open(csv_path) as f:
    for line in f:
        parts=line.strip().split(',')
        if len(parts)!=3: continue
        try:
            rows.append((int(parts[0]), int(parts[1]), parts[2]))
        except: pass

if not rows:
    open(sum_path,'w').write("no data\n"); sys.exit(0)

total = len(rows)
ok    = sum(1 for r in rows if r[2]=="200")
fail  = total - ok

# 注入故障后的窗口
post = [r for r in rows if r[0] >= inject_ts]
# 连续失败的最长持续 ms
max_outage_ms = 0
cur_start = None
for r in post:
    if r[2] != "200":
        if cur_start is None: cur_start = r[0]
        max_outage_ms = max(max_outage_ms, r[0] - cur_start)
    else:
        cur_start = None

# 故障注入到首次再次成功 200 的恢复时间
recover_ms = -1
for r in post:
    if r[2] == "200" and r[0] > inject_ts:
        # 检查是否在故障注入"之后"才出现的成功（注意：故障前 5s 也是 200）
        # 用更稳健的判定：注入后第一次失败的时间起算
        pass
# 重算：注入后第一次失败到下一次成功的间隔
first_fail_after = None
recover_ts = None
for r in post:
    if first_fail_after is None and r[2] != "200":
        first_fail_after = r[0]
    elif first_fail_after is not None and r[2] == "200":
        recover_ts = r[0]; break
if first_fail_after is not None and recover_ts is not None:
    recover_ms = recover_ts - first_fail_after

# 平均时延（成功请求）
ok_lats = [r[1] for r in rows if r[2]=="200"]
avg_lat = sum(ok_lats)/len(ok_lats) if ok_lats else 0

with open(sum_path,'w') as f:
    f.write(f"=== Leader Transfer Demo Summary ({scen}) ===\n")
    f.write(f"total_requests   : {total}\n")
    f.write(f"http_200         : {ok}\n")
    f.write(f"http_non200      : {fail}\n")
    f.write(f"avg_lat_ms_200   : {avg_lat:.1f}\n")
    f.write(f"inject_ts_ms     : {inject_ts}\n")
    f.write(f"max_outage_ms    : {max_outage_ms}\n")
    f.write(f"recover_window_ms: {recover_ms}\n")

print(f"=== {scen} summary ===")
print(open(sum_path).read())
PY

echo "完成。查看:"
echo "  CSV     : $CSV"
echo "  Summary : $SUM"
echo "  日志    : $LOG_DIR/"
