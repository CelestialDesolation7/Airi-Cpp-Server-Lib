#!/usr/bin/env python3
"""ASCII 时延 timeline 对比图：把多份 leader_transfer_*.csv 画在一起。

用法:
    python3 scripts/plot_timeline.py kill9 sigterm transfer
"""
import sys, csv, os

SCENARIOS = sys.argv[1:] or ["kill9", "sigterm"]
WIDTH = 80          # 时间轴宽度（字符）
DURATION_MS = 15000 # 横轴总时长

def load(scen):
    path = f"/tmp/leader_transfer_{scen}.csv"
    if not os.path.exists(path):
        print(f"[skip] {path} 不存在")
        return None
    rows = []
    with open(path) as f:
        for line in f:
            parts = line.strip().split(",")
            if len(parts) != 3: continue
            try: rows.append((int(parts[0]), int(parts[1]), parts[2]))
            except: pass
    return rows

# 字符: . = <50ms 成功；o = 50-200ms 成功；O = 200-1000ms 成功；x = 失败/超时
def cell(rows_in_bucket):
    if not rows_in_bucket:
        return " "
    fail = sum(1 for r in rows_in_bucket if r[2] != "200")
    if fail >= len(rows_in_bucket) * 0.5:  # 半数以上失败：明显故障窗
        return "X"
    max_lat = max(r[1] for r in rows_in_bucket if r[2] == "200")
    if max_lat < 50: return "."
    if max_lat < 200: return "o"
    return "O"

print("\n  Legend: . <50ms  o 50-200ms  O 200-1000ms  X 失败/超时")
print(f"  Time axis: 0 ─────────────────── {DURATION_MS} ms\n")

for scen in SCENARIOS:
    rows = load(scen)
    if rows is None: continue
    bucket_ms = DURATION_MS // WIDTH
    line = []
    for i in range(WIDTH):
        lo = i * bucket_ms
        hi = lo + bucket_ms
        bucket = [r for r in rows if lo <= r[0] < hi]
        line.append(cell(bucket))

    fails = sum(1 for r in rows if r[2] != "200")
    ok    = len(rows) - fails

    # 计算最大故障窗
    max_outage = 0; cur = None
    for r in rows:
        if r[2] != "200":
            if cur is None: cur = r[0]
            max_outage = max(max_outage, r[0] - cur)
        else: cur = None

    print(f"{scen:>10}  |{''.join(line)}|  total={len(rows):>4}  fail={fails:>3}  outage={max_outage}ms")
print()
