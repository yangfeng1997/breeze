#!/bin/bash
# 睡着的线程 / 进程被唤醒要多久：eventfd + epoll、eventfd 直接 read、futex、条件变量。乒乓，两端都真的睡，往返 = 两次唤醒。
# 线程版与进程版（*-proc）都跑，看"拆成两个进程"对唤醒有没有影响。两端各绑一个物理核（cpu2 / cpu4），按 i9-14900KF 定的，换机器请改。
# 每组重复 N 次取中位数，重复在最外层、方式在内层交错。
#
# 用法：bench/run_wake.sh [wake_bench 路径] [重复次数]
#   cmake --preset release -DBREEZE_BUILD_AGENT_BENCH=ON && cmake --build ../../build/cpp-release --target wake_bench

set -u
BIN=${1:-../../build/cpp-release/wake_bench}
REPS=${2:-5}
RAW=${RAW:-/tmp/wake_bench_raw.txt}
MODES="eventfd-epoll eventfd-read futex condvar eventfd-epoll-proc futex-proc"

: >"$RAW"
for rep in $(seq 1 "$REPS"); do
  for mode in $MODES; do
    "$BIN" --mode "$mode" --iters 30000 --cpu-a 2 --cpu-b 4 | sed "s/^RESULT/RESULT rep=$rep/" >>"$RAW" || echo "失败: $mode" >&2
  done
done
echo "环境：$(lscpu | grep -m1 'Model name' | sed 's/.*: *//')，$(uname -r)；两端各绑一个物理核 cpu2 / cpu4，两端都睡；重复 ${REPS} 次取中位数"
RAW="$RAW" MODES="$MODES" python3 - <<'PY'
import os, statistics, collections
rows = collections.defaultdict(list)
for line in open(os.environ["RAW"]):
    if line.startswith("RESULT"):
        kv = dict(p.split("=", 1) for p in line.split()[1:])
        rows[kv["mode"]].append({k: float(v) for k, v in kv.items() if k not in ("mode", "rep")})
print("\n| 方式 | 平均往返 µs | p50 往返 µs | p99 往返 µs | 每跳（p50 的一半）µs | 平均往返各轮范围 | 轮数 |")
print("|---|---:|---:|---:|---:|---|---:|")
for m in os.environ["MODES"].split():
    r = rows.get(m)
    if not r:
        continue
    med = lambda f: statistics.median(x[f] for x in r)
    a = sorted(x["avg_rtt_us"] for x in r)
    print(f"| {m} | {med('avg_rtt_us'):.1f} | {med('p50_rtt_us'):.1f} | {med('p99_rtt_us'):.1f} | {med('p50_rtt_us')/2:.1f} | {a[0]:.1f}~{a[-1]:.1f} | {len(r)} |")
PY
