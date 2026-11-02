#!/bin/bash
# 共享内存环 vs 进程内无锁队列：两个线程 / 两个进程之间传消息有多快（只量队列本身，两端都自旋、不走 eventfd）。
#   queue        进程内两个线程，SpscQueue（moodycamel::ReaderWriterQueue），逐条入队
#   ring-thread  共享内存环，同一进程里的两个线程
#   ring-proc    共享内存环，两个进程（fork 后子进程 Attach 重新映射）
# 环另跑"批量 32 条一次发布"的一组（队列没有批量发布）。每组重复 N 次取中位数，重复在最外层、实现在内层交错。
# 两端各绑一个物理核（cpu2 / cpu4，各自的 SMT 兄弟核留空），方案按 i9-14900KF 定的，换机器请改。
#
# 用法：bench/run_ring.sh [ring_bench 路径] [重复次数]
#   SUMMARIZE_ONLY=1 RAW=<已有的原始结果文件> bench/run_ring.sh   只重新汇总，不跑
#   cmake --preset release -DBREEZE_BUILD_AGENT_BENCH=ON && cmake --build ../../build/cpp-release --target ring_bench

set -u
BIN=${1:-../../build/cpp-release/ring_bench}
REPS=${2:-5}
CPU_A=2
CPU_B=4
RAW=${RAW:-/tmp/ring_bench_raw.txt}

stream_count() { case "$1" in 64) echo 4000000 ;; 1024) echo 2000000 ;; *) echo 200000 ;; esac; }

run_all() {
  : >"$RAW"
  local rep size cfg impl batch
  for rep in $(seq 1 "$REPS"); do
    for size in 64 1024 16384; do
      for cfg in "queue 1" "ring-thread 1" "ring-proc 1" "ring-thread 32" "ring-proc 32"; do
        set -- $cfg
        impl=$1 batch=$2
        "$BIN" --impl "$impl" --size "$size" --batch "$batch" --iters 200000 --stream "$(stream_count "$size")" \
          --cpu-a "$CPU_A" --cpu-b "$CPU_B" | sed "s/^RESULT/RESULT rep=$rep/" >>"$RAW" || echo "失败: $cfg size=$size" >&2
      done
    done
  done
}

summarize() {
  RAW="$RAW" python3 - <<'PY'
import os, statistics, collections
rows = collections.defaultdict(list)
for line in open(os.environ["RAW"]):
    if not line.startswith("RESULT"):
        continue
    kv = dict(p.split("=", 1) for p in line.split()[1:])
    rows[(int(kv["size"]), kv["impl"], int(kv["batch"]))].append({k: float(v) for k, v in kv.items() if k not in ("impl", "rep")})
def med(key, field):
    r = rows.get(key)
    return statistics.median(x[field] for x in r) if r else None
def rng(key, field):
    r = sorted(x[field] for x in rows.get(key, []))
    return f"{r[0]:.0f}~{r[-1]:.0f}" if r else "-"
order = [("queue", 1), ("ring-thread", 1), ("ring-proc", 1), ("ring-thread", 32), ("ring-proc", 32)]
for size in (64, 1024, 16384):
    print(f"\n### 消息 {size} 字节\n")
    print("| 实现 | 批量发布 | 平均往返 ns | p50 ns | p99 ns | 单向吞吐 百万条/秒 | GB/s | 平均往返各轮范围 | 轮数 |")
    print("|---|---:|---:|---:|---:|---:|---:|---|---:|")
    for impl, batch in order:
        k = (size, impl, batch)
        if k not in rows:
            continue
        print(f"| {impl} | {batch} | {med(k,'avg_rtt_ns'):.0f} | {med(k,'p50_ns'):.0f} | {med(k,'p99_ns'):.0f} | "
              f"{med(k,'stream_mmsg_s'):.1f} | {med(k,'stream_gb_s'):.2f} | {rng(k,'avg_rtt_ns')} | {len(rows[k])} |")
print("\n### 相对进程内队列（中位数之比）\n")
print("| 消息 | 环（跨进程，逐条发布）往返 | 环（跨进程，批量 32）吞吐 | 环（跨进程，逐条发布）吞吐 | 跨进程 / 同进程（环，往返） |")
print("|---:|---:|---:|---:|---:|")
for size in (64, 1024, 16384):
    q, p1, p32, t1 = (size, "queue", 1), (size, "ring-proc", 1), (size, "ring-proc", 32), (size, "ring-thread", 1)
    if all(k in rows for k in (q, p1, p32, t1)):
        print(f"| {size} | {med(p1,'avg_rtt_ns')/med(q,'avg_rtt_ns'):.2f}× | {med(p32,'stream_mmsg_s')/med(q,'stream_mmsg_s'):.2f}× | "
              f"{med(p1,'stream_mmsg_s')/med(q,'stream_mmsg_s'):.2f}× | {med(p1,'avg_rtt_ns')/med(t1,'avg_rtt_ns'):.2f}× |")
PY
}

if [ "${SUMMARIZE_ONLY:-0}" != 1 ]; then
  run_all
fi
echo "环境：$(lscpu | grep -m1 'Model name' | sed 's/.*: *//')，$(uname -r)；两端各绑一个物理核 cpu$CPU_A / cpu$CPU_B，两端都自旋；重复 ${REPS} 次取中位数"
summarize
