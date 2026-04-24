#!/usr/bin/env bash
# run_bench.sh — 在容器内依次启动两个服务器并用 wrk 压测，输出对比报告
#
# 测试矩阵：
#   - 服务器：bench_server (本项目)、muduo_bench_server (muduo)
#   - wrk 配置：4 线程 × 100 连接 × 30 秒（短包 keep-alive）
#               4 线程 × 1000 连接 × 30 秒（高并发 keep-alive）
#
# 输出位置：/work/results/
#   - bench_server_c100.txt / bench_server_c1000.txt
#   - muduo_bench_server_c100.txt / muduo_bench_server_c1000.txt
#   - summary.md

set -euo pipefail

PORT="${PORT:-9090}"
IO_THREADS="${IO_THREADS:-4}"
WRK_THREADS="${WRK_THREADS:-4}"
DURATION="${DURATION:-30s}"
RESULTS_DIR="${RESULTS_DIR:-/work/results}"

mkdir -p "${RESULTS_DIR}"

echo "================================================================"
echo "  Airi-Cpp-Server-Lib vs muduo  —  Real Benchmark via wrk"
echo "================================================================"
echo "  IO threads per server : ${IO_THREADS}"
echo "  wrk threads           : ${WRK_THREADS}"
echo "  Duration              : ${DURATION}"
echo "  Port                  : ${PORT}"
echo "  Results directory     : ${RESULTS_DIR}"
echo "----------------------------------------------------------------"
echo "  CPU info:"
nproc
echo "----------------------------------------------------------------"

run_one() {
    local server_bin="$1"
    local label="$2"

    echo
    echo "=== Starting ${label} ==="
    "${server_bin}" "${PORT}" "${IO_THREADS}" >"${RESULTS_DIR}/${label}.log" 2>&1 &
    local pid=$!

    # 等待端口就绪
    for i in $(seq 1 20); do
        if curl -sf "http://127.0.0.1:${PORT}/" -o /dev/null; then
            break
        fi
        sleep 0.2
    done
    if ! curl -sf "http://127.0.0.1:${PORT}/" -o /dev/null; then
        echo "ERROR: ${label} failed to start"
        kill -9 "${pid}" 2>/dev/null || true
        return 1
    fi
    echo "${label} pid=${pid} ready."

    # 暖机 3 秒
    wrk -t "${WRK_THREADS}" -c 100 -d 3s "http://127.0.0.1:${PORT}/" >/dev/null 2>&1 || true

    # 测试 1：100 并发 keep-alive
    echo "--- ${label}: wrk -t${WRK_THREADS} -c100 -d${DURATION} ---"
    wrk -t "${WRK_THREADS}" -c 100 -d "${DURATION}" --latency \
        "http://127.0.0.1:${PORT}/" | tee "${RESULTS_DIR}/${label}_c100.txt"

    # 测试 2：1000 并发 keep-alive
    echo "--- ${label}: wrk -t${WRK_THREADS} -c1000 -d${DURATION} ---"
    wrk -t "${WRK_THREADS}" -c 1000 -d "${DURATION}" --latency \
        "http://127.0.0.1:${PORT}/" | tee "${RESULTS_DIR}/${label}_c1000.txt"

    # 关闭服务器
    kill -INT "${pid}" 2>/dev/null || true
    sleep 1
    kill -9 "${pid}" 2>/dev/null || true
    wait "${pid}" 2>/dev/null || true
}

run_one /usr/local/bin/bench_server         "bench_server"
run_one /usr/local/bin/muduo_bench_server   "muduo_bench_server"

# ── 生成 summary.md ──────────────────────────────────────────────────
extract() {
    # $1 = file, $2 = field key, e.g. "Requests/sec:" or "50%"
    grep -E "$2" "$1" | head -n1 | awk '{print $NF}'
}

extract_p() {
    # 解析 wrk --latency 的 P50/P75/P90/P99
    local f="$1" pct="$2"
    grep -E "^[[:space:]]+${pct}%" "$f" | head -n1 | awk '{print $2}'
}

summary="${RESULTS_DIR}/summary.md"
{
    echo "# Airi-Cpp-Server-Lib vs muduo — Real wrk Benchmark"
    echo
    echo "- Date: $(date -u +%FT%TZ)"
    echo "- Container OS: $(. /etc/os-release && echo "${PRETTY_NAME}")"
    echo "- Kernel: $(uname -r)"
    echo "- CPU cores: $(nproc)"
    echo "- IO threads per server: ${IO_THREADS}"
    echo "- wrk threads: ${WRK_THREADS}, duration: ${DURATION}"
    echo "- Workload: HTTP/1.1 keep-alive, GET /, response body 6 bytes"
    echo
    echo "## c=100"
    echo
    echo "| Server | RPS | Avg latency | P50 | P75 | P90 | P99 |"
    echo "|--------|-----|------------|-----|-----|-----|-----|"
    for s in bench_server muduo_bench_server; do
        f="${RESULTS_DIR}/${s}_c100.txt"
        rps=$(extract "$f" "Requests/sec:")
        avg=$(grep -E "^[[:space:]]+Latency" "$f" | head -n1 | awk '{print $2}')
        p50=$(extract_p "$f" 50)
        p75=$(extract_p "$f" 75)
        p90=$(extract_p "$f" 90)
        p99=$(extract_p "$f" 99)
        echo "| ${s} | ${rps} | ${avg} | ${p50} | ${p75} | ${p90} | ${p99} |"
    done
    echo
    echo "## c=1000"
    echo
    echo "| Server | RPS | Avg latency | P50 | P75 | P90 | P99 |"
    echo "|--------|-----|------------|-----|-----|-----|-----|"
    for s in bench_server muduo_bench_server; do
        f="${RESULTS_DIR}/${s}_c1000.txt"
        rps=$(extract "$f" "Requests/sec:")
        avg=$(grep -E "^[[:space:]]+Latency" "$f" | head -n1 | awk '{print $2}')
        p50=$(extract_p "$f" 50)
        p75=$(extract_p "$f" 75)
        p90=$(extract_p "$f" 90)
        p99=$(extract_p "$f" 99)
        echo "| ${s} | ${rps} | ${avg} | ${p50} | ${p75} | ${p90} | ${p99} |"
    done
} >"${summary}"

echo
echo "================================================================"
echo "  Summary written to ${summary}"
echo "================================================================"
cat "${summary}"
