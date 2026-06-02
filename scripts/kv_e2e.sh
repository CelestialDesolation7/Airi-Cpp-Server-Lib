#!/bin/bash
# kv_e2e.sh —— 分布式 KV 节点端到端验证脚本
#
# 用法：bash scripts/kv_e2e.sh
#
# 测试场景：
#   1. 启动 3 节点集群（节点 0/1/2，端口 8901/8902/8903）
#   2. 等待 Leader 选出，写入 key=hello value=world
#   3. 从 3 个节点各自读取，验证复制成功
#   4. 找到 Leader 节点进程并 kill 它
#   5. 等待新 Leader 选出，再次从存活节点读取
#   6. 重启原 Leader，等待它追赶，再次读取验证一致性
#   7. 测试 DELETE 命令：删除 key 后验证 404

set -euo pipefail

KV_NODE="./build/examples/kv_node"
BASE_HTTP=8901
BASE_RPC=19001
NODES=3

# 颜色输出
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

pass() { echo -e "${GREEN}[PASS]${NC} $*"; }
fail() { echo -e "${RED}[FAIL]${NC} $*"; exit 1; }
info() { echo -e "${YELLOW}[INFO]${NC} $*"; }

# 检查二进制存在
if [[ ! -x "$KV_NODE" ]]; then
    fail "找不到 $KV_NODE，请先运行: cmake --build build --target kv_node"
fi

# ── §1 启动 3 节点集群 ────────────────────────────────────────────────────────
info "启动 3 节点 KV 集群 ..."
mkdir -p /tmp/kv_e2e_state

PIDS=()
for i in 0 1 2; do
    "$KV_NODE" --id "$i" --nodes 3 \
        2>"/tmp/kv_e2e_state/node${i}.log" &
    PIDS+=($!)
    info "  节点 $i PID=${PIDS[$i]}  HTTP=:$((BASE_HTTP+i))  RPC=:$((BASE_RPC+i))"
done

cleanup() {
    info "清理进程 ..."
    for pid in "${PIDS[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    # 清理重启的节点
    kill "$RESTART_PID" 2>/dev/null || true
}
RESTART_PID=0
trap cleanup EXIT

# ── §2 等待 Leader 选出（最多 10s），写入第一个 key ──────────────────────────
info "等待 Leader 选出 ..."
LEADER_PORT=""
for attempt in $(seq 1 20); do
    sleep 0.5
    for i in 0 1 2; do
        port=$((BASE_HTTP+i))
        state=$(curl -sf "http://127.0.0.1:${port}/admin/raft" 2>/dev/null \
                | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['state'])" \
                2>/dev/null || true)
        if [[ "$state" == "Leader" ]]; then
            LEADER_PORT=$port
            LEADER_ID=$i
            pass "Leader 已选出: 节点 $i (HTTP :${port})"
            break 2
        fi
    done
done
[[ -n "$LEADER_PORT" ]] || fail "10 秒内未选出 Leader"

# ── §3 写入 key ───────────────────────────────────────────────────────────────
info "写入 hello=world ..."
HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" \
    -X PUT "http://127.0.0.1:${LEADER_PORT}/kv/hello" \
    -d "world")
[[ "$HTTP_CODE" == "200" ]] || fail "PUT /kv/hello 失败 (HTTP $HTTP_CODE)"
pass "PUT /kv/hello=world → 200"

# ── §4 从 3 个节点各自读取（给一点时间让复制传播） ─────────────────────────
sleep 0.3
info "从 3 个节点读取 hello ..."
for i in 0 1 2; do
    port=$((BASE_HTTP+i))
    val=$(curl -sf "http://127.0.0.1:${port}/kv/hello" \
          | python3 -c "import sys,json; print(json.load(sys.stdin)['value'])" \
          2>/dev/null || echo "ERROR")
    if [[ "$val" == "world" ]]; then
        pass "  节点 $i GET /kv/hello = world"
    else
        fail "  节点 $i GET /kv/hello = '$val' (期望 world)"
    fi
done

# ── §5 Kill Leader，等待新 Leader ─────────────────────────────────────────────
info "Kill Leader (节点 ${LEADER_ID}, PID=${PIDS[$LEADER_ID]}) ..."
kill "${PIDS[$LEADER_ID]}"
# 从 PIDS 中标记已停止
OLD_LEADER_PID=${PIDS[$LEADER_ID]}
PIDS[$LEADER_ID]=0

info "等待新 Leader 选出 ..."
NEW_LEADER_PORT=""
for attempt in $(seq 1 30); do
    sleep 0.5
    for i in 0 1 2; do
        [[ $i -eq $LEADER_ID ]] && continue
        port=$((BASE_HTTP+i))
        state=$(curl -sf "http://127.0.0.1:${port}/admin/raft" 2>/dev/null \
                | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['state'])" \
                2>/dev/null || true)
        if [[ "$state" == "Leader" ]]; then
            NEW_LEADER_PORT=$port
            NEW_LEADER_ID=$i
            pass "新 Leader 已选出: 节点 $i (HTTP :${port})"
            break 2
        fi
    done
done
[[ -n "$NEW_LEADER_PORT" ]] || fail "Leader 故障后 15 秒内未选出新 Leader"

# ── §6 从存活节点读取，验证数据持久 ─────────────────────────────────────────
for i in 0 1 2; do
    [[ $i -eq $LEADER_ID ]] && continue
    port=$((BASE_HTTP+i))
    val=$(curl -sf "http://127.0.0.1:${port}/kv/hello" \
          | python3 -c "import sys,json; print(json.load(sys.stdin)['value'])" \
          2>/dev/null || echo "ERROR")
    if [[ "$val" == "world" ]]; then
        pass "  Leader 故障后节点 $i GET /kv/hello = world"
    else
        fail "  Leader 故障后节点 $i GET /kv/hello = '$val' (期望 world)"
    fi
done

# ── §7 重启原 Leader，等待追赶 ───────────────────────────────────────────────
info "重启节点 ${LEADER_ID} ..."
"$KV_NODE" --id "$LEADER_ID" --nodes 3 \
    2>"/tmp/kv_e2e_state/node${LEADER_ID}_restart.log" &
RESTART_PID=$!
PIDS[$LEADER_ID]=$RESTART_PID
sleep 3  # 给足时间追赶日志

val=$(curl -sf "http://127.0.0.1:$((BASE_HTTP+LEADER_ID))/kv/hello" \
      | python3 -c "import sys,json; print(json.load(sys.stdin)['value'])" \
      2>/dev/null || echo "ERROR")
if [[ "$val" == "world" ]]; then
    pass "重启后节点 ${LEADER_ID} 追赶完成，GET /kv/hello = world"
else
    fail "重启后节点 ${LEADER_ID} GET /kv/hello = '$val' (期望 world)"
fi

# ── §8 测试 DELETE ────────────────────────────────────────────────────────────
info "测试 DELETE /kv/hello ..."
HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" \
    -X DELETE "http://127.0.0.1:${NEW_LEADER_PORT}/kv/hello")
[[ "$HTTP_CODE" == "200" ]] || fail "DELETE /kv/hello 失败 (HTTP $HTTP_CODE)"
pass "DELETE /kv/hello → 200"

sleep 0.3
for i in 0 1 2; do
    port=$((BASE_HTTP+i))
    HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" \
        "http://127.0.0.1:${port}/kv/hello" 2>/dev/null || echo "000")
    if [[ "$HTTP_CODE" == "404" ]]; then
        pass "  节点 $i GET /kv/hello (已删除) → 404"
    else
        fail "  节点 $i GET /kv/hello 期望 404，得到 $HTTP_CODE"
    fi
done

echo ""
pass "=== 全部 8 项场景通过 ==="
