#!/bin/bash
# 连接规模的详细性能：1000 / 5000 / 10000 / 100000 条连接，64B 消息。按业界常见的指标分四组：
#   1. 建连：建立 N 条连接的耗时与速率（压测客户端每线程顺序 connect，所以是下限，不是服务端 accept 的上限）
#   2. 每连接内存与空闲 CPU（--mem-probe：服务端进程 RSS 增量 ÷ N；N 条空闲连接下服务端线程的 CPU）
#   3. 同一负载（总共 5 万 msg/s，各实现都在满载以下）下连接数的影响：每条连接每秒发 50000 / N 条，
#      p50 / p90 / p99 / p99.9 / max 与服务端 CPU
#   4. 负载扫描（10000 条连接，总负载 2.5 万 → 20 万 msg/s）：找拐点——最高的、达标（实际吞吐 ≥ 99% 且 p99 ≤ 5ms）的负载
# 定速是均匀的开环节拍（每条连接按固定间隔发送、各连接的起点错开，纳秒精度的定时），不是闭环全速：
# 负载由 连接数 × 速率 决定，与服务端快慢无关。均匀到达时服务端线程几乎每条消息都要醒一次，
# 每条消息的 CPU 比闭环全速（会成批处理）高得多，所以满载的拐点比闭环全速的吞吐低。
# UDP 最多约 2.5 万条连接（本机的临时端口范围与 bind 的限制，见 net_bench.cpp 的 kScaleThreshold 旁注释），
# 所以 UDP 用 1000 / 5000 / 10000 / 25000。
# 实现：tcp = raw / muduo / breeze / breeze-mainloop；uds = raw / breeze / breeze-mainloop；
#       udp = raw-udp / rudp / rudp-mainloop。重复在最外层、实现在内层交错，取中位数。
# 用法：bench/run_scale.sh [net_bench 路径] [第 3 组重复次数] [第 4 组重复次数]
#   SUMMARIZE_ONLY=1 RAW=<结果文件> RAW_MEM=<内存结果文件> bench/run_scale.sh   只重新汇总
# 绑核同 run_baseline.sh（服务端 cpu2、主循环 cpu12、压测线程 cpu4/6/8/10）。

set -u
BIN=${1:-../../build/cpp-release/net_bench}
REPS=${2:-3}
KNEE_REPS=${3:-2}
RAW=${RAW:-/tmp/net_bench_scale_raw.txt}
RAW_MEM=${RAW_MEM:-/tmp/net_bench_scale_mem.txt}
COMMON="--size 64 --duration 3 --warmup 1 --client-threads 4 --server-cpu 2 --mainloop-cpu 12 --client-cpus 4,6,8,10"

impls_for() {
  case "$1" in
    tcp) echo raw muduo breeze breeze-mainloop ;;
    uds) echo raw breeze breeze-mainloop ;;
    udp) echo raw-udp rudp rudp-mainloop ;;
  esac
}
conns_for() { # 传输 → 连接数列表
  if [ "$1" = udp ]; then echo "1000 5000 10000 25000"; else echo "1000 5000 10000 100000"; fi
}

run_one() { # transport impl conns rate
  local t=$1 i=$2 c=$3 r=$4 out
  out=$(timeout 300 "$BIN" --impl "$i" --transport "$t" --conns "$c" --rate "$r" $COMMON 2>&1 | grep -E '^(RESULT|EXTRA)')
  if [ -z "$out" ]; then
    echo "FAILED: $t $i conns=$c rate=$r" >&2
    return
  fi
  echo $out | sed 's/ EXTRA / /' >> "$RAW"
}

run_all() {
  : > "$RAW"
  if [ -z "${SKIP_MEM:-}" ]; then
  : > "$RAW_MEM"
  # 2. 内存与空闲 CPU
  for t in tcp uds udp; do
    for c in $(conns_for $t); do
      for i in $(impls_for $t); do
        timeout 300 "$BIN" --impl "$i" --transport "$t" --mem-probe "$c" --server-cpu 2 --mainloop-cpu 12 2>&1 \
          | grep '^MEM' >> "$RAW_MEM" || echo "FAILED mem: $t $i $c" >&2
      done
    done
  done
  echo "内存探测完成" >&2
  fi
  # 3. 同负载（10 万 msg/s）下的连接数
  for rep in $(seq 1 "$REPS"); do
    for t in tcp uds udp; do
      for c in $(conns_for $t); do
        rate=$(python3 -c "print(50000/$c)")
        for i in $(impls_for $t); do
          run_one "$t" "$i" "$c" "$rate"
        done
      done
    done
    echo "第 3 组 $rep/$REPS 轮完成" >&2
  done
  # 4. 负载扫描（10000 条连接）
  for rep in $(seq 1 "$KNEE_REPS"); do
    for rate in 2.5 7.5 10 12.5 15 20; do # 5 条/s（5 万）已在第 3 组里
      for t in tcp uds udp; do
        for i in $(impls_for $t); do
          run_one "$t" "$i" 10000 "$rate"
        done
      done
    done
    echo "第 4 组 $rep/$KNEE_REPS 轮完成" >&2
  done
}

[ -z "${SUMMARIZE_ONLY:-}" ] && run_all

python3 -I - "$RAW" "$RAW_MEM" <<'EOF'
import statistics, sys
from collections import defaultdict

rows = defaultdict(list)
for line in open(sys.argv[1], encoding="utf-8"):
    kv = dict(p.split("=", 1) for p in line.split()[1:])
    load = round(int(kv["conns"]) * float(kv["rate"]) / 1000) * 1000  # 设定的总负载 msg/s（取整到千）
    rows[(kv["transport"], kv["impl"], int(kv["conns"]), load)].append(
        {k: float(v) for k, v in kv.items() if k not in ("impl", "transport")})
mem = {}
for line in open(sys.argv[2], encoding="utf-8"):
    kv = dict(p.split("=", 1) for p in line.split()[1:])
    mem[(kv["transport"], kv["impl"], int(kv["conns"]))] = kv

def med(key, field):
    vals = [r[field] for r in rows.get(key, []) if field in r]
    return statistics.median(vals) if vals else None

SPECS = [("tcp", "TCP", ["raw", "muduo", "breeze", "breeze-mainloop"], [1000, 5000, 10000, 100000]),
         ("uds", "UDS", ["raw", "breeze", "breeze-mainloop"], [1000, 5000, 10000, 100000]),
         ("udp", "UDP", ["raw-udp", "rudp", "rudp-mainloop"], [1000, 5000, 10000, 25000])]
LOAD = 50000

def fmt_n(n):
    return f"{n:,}"

print("### 1. 建连（建立 N 条连接的耗时 s；括号内是每秒建立的连接数，压测客户端每线程顺序 connect，是下限）\n")
for t, name, impls, ns in SPECS:
    print(f"**{name}**\n")
    print("| 实现 | " + " | ".join(fmt_n(n) for n in ns) + " |")
    print("|---|" + "---|" * len(ns))
    for i in impls:
        cells = []
        for n in ns:
            s = med((t, i, n, LOAD), "setup_s")
            cells.append("—" if s is None else f"{s:.2f}（{n / s:,.0f}/s）")
        print(f"| {i} | " + " | ".join(cells) + " |")
    print()

print("### 2. 每连接内存（B）与空闲 CPU（N 条空闲连接下服务端线程的 CPU）\n")
for t, name, impls, ns in SPECS:
    print(f"**{name}**\n")
    print("| 实现 | " + " | ".join(fmt_n(n) for n in ns) + " |")
    print("|---|" + "---|" * len(ns))
    for i in impls:
        cells = []
        for n in ns:
            m = mem.get((t, i, n))
            cells.append("—" if m is None else f"{float(m['bytes_per_conn']):,.0f} B · {float(m['idle_cpu_pct']):.2f}%")
        print(f"| {i} | " + " | ".join(cells) + " |")
    print()

print("### 3. 同一负载（总共 5 万 msg/s）下连接数的影响（p50 / p99 / p99.9 µs · 服务端 IO 线程 CPU，括号为主循环线程；实测吞吐不足 99% 或丢包时标注）\n")
for t, name, impls, ns in SPECS:
    print(f"**{name}**\n")
    print("| 实现 | " + " | ".join(f"{fmt_n(n)} 连接 × {LOAD / n:g}/s" for n in ns) + " |")
    print("|---|" + "---|" * len(ns))
    for i in impls:
        cells = []
        for n in ns:
            k = (t, i, n, LOAD)
            if k not in rows:
                cells.append("—")
                continue
            c, m = med(k, "srv_cpu_pct"), med(k, "main_cpu_pct")
            tp, drops = med(k, "msgs_per_s"), med(k, "drops_per_s") or 0
            s = f"{med(k, 'p50_us'):.0f} / {med(k, 'p99_us'):.0f} / {med(k, 'p999_us'):.0f} · {c:.0f}%"
            if m is not None:
                s += f"（{m:.0f}%）"
            if tp < 0.99 * LOAD:
                s += f"，**只达 {tp / LOAD * 100:.0f}%**"
            if drops >= 10:
                s += f"，丢包 {drops:,.0f}/s"
            cells.append(s)
        print(f"| {i} | " + " | ".join(cells) + " |")
    print()

print("### 4. 负载扫描（10000 条连接；p99 µs · 服务端 IO 线程 CPU；* = 实测吞吐不足设定的 99% 或 p99 > 5ms，即不达标）\n")
loads = [25000, 50000, 75000, 100000, 125000, 150000, 200000]
print("| 传输 | 实现 | " + " | ".join(f"{l // 1000}k msg/s" for l in loads) + " | 最高达标负载 |")
print("|---|---|" + "---|" * (len(loads) + 1))
for t, name, impls, _ in SPECS:
    for i in impls:
        cells, best = [], None
        for l in loads:
            k = (t, i, 10000, l)
            if k not in rows:
                cells.append("—")
                continue
            tp, p99, c = med(k, "msgs_per_s"), med(k, "p99_us"), med(k, "srv_cpu_pct")
            ok = tp >= 0.99 * l and p99 <= 5000
            if ok:
                best = l
            cells.append(f"{p99:.0f} · {c:.0f}%" + ("" if ok else "*"))
        print(f"| {name} | {i} | " + " | ".join(cells) + f" | {'< 2.5 万' if best is None else f'{best // 1000}k'} |")
EOF
