#!/bin/bash
# 跑完整基线矩阵：传输(tcp/uds) × 消息大小(64/1024) × 连接数(1/64/10000) × 实现(raw/muduo/breeze/breeze-fixed)。
# 每组重复 N 次取中位数；重复在最外层、实现在内层交错，让温度、后台噪声对各实现的影响大致相同。
# muduo 只支持 TCP。输出 markdown 表到 stdout，原始 RESULT 行写到 $RAW。
#
# 用法：bench/run_baseline.sh [net_bench 路径] [重复次数] [每组计时秒数]
#   SUMMARIZE_ONLY=1 RAW=<已有的原始结果文件> bench/run_baseline.sh   只重新汇总，不跑
#   cmake --preset release -DBREEZE_BUILD_BENCH=ON && cmake --build ../../build/cpp-release --target net_bench
# 绑核：服务端独占 cpu2，压测线程占 cpu 4/6/8/10；cpu 号两两是 SMT 兄弟，各自的兄弟核留空，避免互相干扰。
# 绑核方案按 i9-14900KF（逻辑核 0-1、2-3… 为一对）定的，换机器请改下面的 SERVER_CPU / CLIENT_CPUS。

set -u
BIN=${1:-../../build/cpp-release/net_bench}
REPS=${2:-3}
DURATION=${3:-3}
SERVER_CPU=2
CLIENT_CPUS=4,6,8,10
RAW=${RAW:-/tmp/net_bench_raw.txt}

run_all() {
: > "$RAW"
for rep in $(seq 1 "$REPS"); do
  for transport in tcp uds; do
    for size in 64 1024; do
      for conns in 1 64 10000; do
        for impl in raw muduo breeze breeze-fixed; do
          [ "$impl" = muduo ] && [ "$transport" = uds ] && continue
          "$BIN" --impl $impl --transport $transport --size $size --conns $conns \
                 --duration "$DURATION" --warmup 1 --client-threads 4 \
                 --server-cpu $SERVER_CPU --client-cpus $CLIENT_CPUS 2>&1 | grep '^RESULT' >> "$RAW" \
            || echo "FAILED: rep=$rep $impl $transport size=$size conns=$conns" >&2
        done
      done
    done
  done
  echo "第 $rep/$REPS 轮完成" >&2
done
}

[ -z "${SUMMARIZE_ONLY:-}" ] && run_all

python3 -I - "$RAW" <<'EOF'
import re, statistics, sys
from collections import defaultdict

rows = defaultdict(list)
for line in open(sys.argv[1], encoding="utf-8"):
    kv = dict(p.split("=", 1) for p in line.split()[1:])
    key = (kv["transport"], int(kv["size"]), int(kv["conns"]), kv["impl"])
    rows[key].append({k: float(v) for k, v in kv.items() if k not in ("impl", "transport")})

def med(key, field):
    vals = [r[field] for r in rows.get(key, [])]
    return statistics.median(vals) if vals else None

def spread(key, field):
    vals = [r[field] for r in rows.get(key, [])]
    return (max(vals) - min(vals)) / statistics.median(vals) * 100 if len(vals) > 1 else 0.0

for transport in ("tcp", "uds"):
    impls = ["raw", "muduo", "breeze", "breeze-fixed"] if transport == "tcp" else ["raw", "breeze", "breeze-fixed"]
    print(f"\n### {transport.upper()}（吞吐 msg/s，中位数；括号内为相对 raw；延迟 µs 为 p50 / p99）\n")
    head = "| 消息 | 连接 | " + " | ".join(impls) + " | 服务端 CPU(breeze) |"
    print(head)
    print("|" + "---|" * (len(impls) + 3))
    for size in (64, 1024):
        for conns in (1, 64, 10000):
            base = med((transport, size, conns, "raw"), "msgs_per_s")
            cells = []
            for impl in impls:
                k = (transport, size, conns, impl)
                m = med(k, "msgs_per_s")
                if m is None:
                    cells.append("—")
                    continue
                rel = "" if impl == "raw" or not base else f"（{m / base * 100:.0f}%）"
                cells.append(f"{m:,.0f}{rel}<br>{med(k, 'p50_us'):.0f} / {med(k, 'p99_us'):.0f}")
            cpu = med((transport, size, conns, "breeze"), "srv_cpu_pct")
            print(f"| {size}B | {conns} | " + " | ".join(cells) + f" | {'—' if cpu is None else f'{cpu:.0f}%'} |")

print("\n### 同一配置各次重复之间的波动（(最大-最小)/中位数，%）——差距小于它就不能下结论\n")
worst = defaultdict(float)
for (transport, size, conns, impl) in rows:
    worst[impl] = max(worst[impl], spread((transport, size, conns, impl), "msgs_per_s"))
print("| " + " | ".join(worst) + " |")
print("|" + "---|" * len(worst))
print("| " + " | ".join(f"最大 {v:.0f}%" for v in worst.values()) + " |")
EOF
