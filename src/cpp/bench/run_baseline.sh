#!/bin/bash
# 跑完整基线矩阵：传输(tcp/uds/udp) × 消息大小(64/1024) × 连接数(1/64/10000) × 实现。
#   tcp：raw / muduo / breeze / breeze-fixed / breeze-mainloop；uds：raw / breeze / breeze-fixed / breeze-mainloop；
#   udp：raw-udp（裸 UDP 回显，UDP 的上限）/ rudp（本库，flush_on_send）/ rudp-mainloop（同 rudp，经主循环）/ rudp-batched（本库默认的批量配置）。
# TRANSPORTS 选传输（默认 "tcp uds udp"）；udp 与 tcp 都跑时，汇总里多一张 TCP 与 UDP 的对照表。
# 主矩阵之外还有一组"会话多、活跃少"（SCALE=1，默认开）：10000 条连接已建立、只有 64 条 ping-pong，
# 测空闲会话对活跃连接的拖累（rudp 每 10ms 一拍的定时器要走遍所有会话，TCP 的空闲连接不占 CPU）。
# UDP 没有背压：连接数一多，服务端接收缓冲（内核封顶约 416KB）装不下，多出来的请求被丢、靠重传补，
# 结果里的 drops_per_s 是测量窗口内全系统的 UDP RcvbufErrors；主矩阵的 10000 条全活跃那一行是"过载区"，别拿它当容量。
# 每组重复 N 次取中位数；重复在最外层、实现在内层交错，让温度、后台噪声对各实现的影响大致相同。
# muduo 只支持 TCP。输出 markdown 表到 stdout，原始 RESULT 行写到 $RAW。
#
# 用法：[TRANSPORTS="tcp udp"] bench/run_baseline.sh [net_bench 路径] [重复次数] [每组计时秒数]
#   SUMMARIZE_ONLY=1 RAW=<已有的原始结果文件> bench/run_baseline.sh   只重新汇总，不跑
#   cmake --preset release -DBREEZE_BUILD_BENCH=ON && cmake --build ../../build/cpp-release --target net_bench
# 绑核：服务端 IO 线程独占 cpu2，breeze-mainloop 的主循环线程占 cpu12，压测线程占 cpu 4/6/8/10；cpu 号两两是 SMT 兄弟，各自的兄弟核留空，避免互相干扰。
# 绑核方案按 i9-14900KF（逻辑核 0-1、2-3… 为一对）定的，换机器请改下面的 SERVER_CPU / MAINLOOP_CPU / CLIENT_CPUS。

set -u
BIN=${1:-../../build/cpp-release/net_bench}
REPS=${2:-3}
DURATION=${3:-3}
SERVER_CPU=2
MAINLOOP_CPU=12
CLIENT_CPUS=4,6,8,10
RAW=${RAW:-/tmp/net_bench_raw.txt}
SCALE=${SCALE:-1}
TRANSPORTS=${TRANSPORTS:-tcp uds udp}

impls_for() {
  case "$1" in
    tcp) echo raw muduo breeze breeze-fixed breeze-mainloop ;;
    uds) echo raw breeze breeze-fixed breeze-mainloop ;;
    udp) echo raw-udp rudp rudp-mainloop rudp-batched ;;
  esac
}

run_all() {
: > "$RAW"
for rep in $(seq 1 "$REPS"); do
  for transport in $TRANSPORTS; do
    for size in 64 1024; do
      for conns in 1 64 10000; do
        for impl in $(impls_for "$transport"); do
          "$BIN" --impl $impl --transport $transport --size $size --conns $conns \
                 --duration "$DURATION" --warmup 1 --client-threads 4 \
                 --server-cpu $SERVER_CPU --mainloop-cpu $MAINLOOP_CPU --client-cpus $CLIENT_CPUS 2>&1 | grep '^RESULT' >> "$RAW" \
            || echo "FAILED: rep=$rep $impl $transport size=$size conns=$conns" >&2
        done
      done
    done
  done
  if [ "$SCALE" = 1 ]; then
    for transport in $TRANSPORTS; do
      [ "$transport" = uds ] && continue
      for size in 64 1024; do
        for impl in $(impls_for "$transport"); do
          case "$impl" in breeze-fixed|breeze-mainloop|rudp-mainloop|rudp-batched) continue ;; esac
          "$BIN" --impl $impl --transport $transport --size $size --conns 10000 --active 64 \
                 --duration "$DURATION" --warmup 1 --client-threads 4 \
                 --server-cpu $SERVER_CPU --mainloop-cpu $MAINLOOP_CPU --client-cpus $CLIENT_CPUS 2>&1 | grep '^RESULT' >> "$RAW" \
            || echo "FAILED: rep=$rep $impl $transport size=$size conns=10000 active=64" >&2
        done
      done
    done
  fi
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
    # 没有 active 字段的旧结果：全部连接活跃
    key = (kv["transport"], int(kv["size"]), int(kv["conns"]), int(kv.get("active", kv["conns"])), kv["impl"])
    rows[key].append({k: float(v) for k, v in kv.items() if k not in ("impl", "transport")})

def med(key, field):
    vals = [r[field] for r in rows.get(key, []) if field in r]
    return statistics.median(vals) if vals else None

def spread(key, field):
    vals = [r[field] for r in rows.get(key, [])]
    return (max(vals) - min(vals)) / statistics.median(vals) * 100 if len(vals) > 1 else 0.0

SPECS = {
    "tcp": (["raw", "muduo", "breeze", "breeze-fixed", "breeze-mainloop"], "raw", "breeze"),
    "uds": (["raw", "breeze", "breeze-fixed", "breeze-mainloop"], "raw", "breeze"),
    "udp": (["raw-udp", "rudp", "rudp-mainloop", "rudp-batched"], "raw-udp", "rudp"),
}
present = {k[0] for k in rows}
for transport in ("tcp", "uds", "udp"):
    if transport not in present:
        continue
    impls, base_impl, cpu_impl = SPECS[transport]
    print(f"\n### {transport.upper()}（吞吐 msg/s，中位数；括号内为相对 {base_impl}；延迟 µs 为 p50 / p99）\n")
    extra = " 丢包/s(rudp) |" if transport == "udp" else ""
    print("| 消息 | 连接 | " + " | ".join(impls) + f" | 服务端 CPU({cpu_impl}) |{extra}")
    print("|" + "---|" * (len(impls) + 3 + (1 if transport == "udp" else 0)))
    for size in (64, 1024):
        for conns in (1, 64, 10000):
            base = med((transport, size, conns, conns, base_impl), "msgs_per_s")
            cells = []
            for impl in impls:
                k = (transport, size, conns, conns, impl)
                m = med(k, "msgs_per_s")
                if m is None:
                    cells.append("—")
                    continue
                rel = "" if impl == base_impl or not base else f"（{m / base * 100:.0f}%）"
                cells.append(f"{m:,.0f}{rel}<br>{med(k, 'p50_us'):.0f} / {med(k, 'p99_us'):.0f}")
            cpu = med((transport, size, conns, conns, cpu_impl), "srv_cpu_pct")
            tail = ""
            if transport == "udp":
                d = med((transport, size, conns, conns, "rudp"), "drops_per_s")
                tail = f" {'—' if d is None else f'{d:,.0f}'} |"
            print(f"| {size}B | {conns} | " + " | ".join(cells) + f" | {'—' if cpu is None else f'{cpu:.0f}%'} |{tail}")

scale = [k for k in rows if k[3] != k[2]]
if scale:
    print("\n### 会话多、活跃少：10000 条连接已建立，只有 64 条 ping-pong（吞吐 msg/s 中位数；变化是相对同传输同大小下 64 连接全活跃；延迟 µs 为 p50 / p99）\n")
    print("| 消息 | 传输 | 实现 | 64 连接全活跃 | 10000 连接 / 64 活跃 | 变化 | 服务端 CPU |")
    print("|---|---|---|---|---|---|---|")
    for size in (64, 1024):
        for transport, impls in (("tcp", ["raw", "muduo", "breeze"]), ("udp", ["raw-udp", "rudp"])):
            for impl in impls:
                a = (transport, size, 64, 64, impl)
                b = (transport, size, 10000, 64, impl)
                ma, mb = med(a, "msgs_per_s"), med(b, "msgs_per_s")
                if mb is None:
                    continue
                chg = "—" if ma is None else f"{(mb / ma - 1) * 100:+.0f}%"
                fa = "—" if ma is None else f"{ma:,.0f}<br>{med(a, 'p50_us'):.0f} / {med(a, 'p99_us'):.0f}"
                fb = f"{mb:,.0f}<br>{med(b, 'p50_us'):.0f} / {med(b, 'p99_us'):.0f}"
                cpu = med(b, "srv_cpu_pct")
                print(f"| {size}B | {transport} | {impl} | {fa} | {fb} | {chg} | {cpu:.0f}% |")

if "tcp" in present and "udp" in present:
    print("\n### TCP 与 UDP 对照（rudp 相对 TCP 上的 muduo / breeze；吞吐 msg/s，中位数；延迟 µs 为 p50 / p99）\n")
    print("| 消息 | 连接 | muduo(TCP) | breeze(TCP) | raw-udp | rudp | rudp / breeze | rudp / muduo | rudp-batched |")
    print("|" + "---|" * 9)
    for size in (64, 1024):
        for conns in (1, 64, 10000):
            def cell(transport, impl):
                k = (transport, size, conns, conns, impl)
                m = med(k, "msgs_per_s")
                return None if m is None else (m, med(k, "p50_us"), med(k, "p99_us"))
            mu, br, ru, rw, rb = cell("tcp", "muduo"), cell("tcp", "breeze"), cell("udp", "rudp"), cell("udp", "raw-udp"), cell("udp", "rudp-batched")
            fmt = lambda c: "—" if c is None else f"{c[0]:,.0f}<br>{c[1]:.0f} / {c[2]:.0f}"
            ratio = lambda a, b: "—" if a is None or b is None else f"{a[0] / b[0] * 100:.0f}%"
            print(f"| {size}B | {conns} | {fmt(mu)} | {fmt(br)} | {fmt(rw)} | {fmt(ru)} | {ratio(ru, br)} | {ratio(ru, mu)} | {fmt(rb)} |")

print("\n### 同一配置各次重复之间的波动（(最大-最小)/中位数，%）——差距小于它就不能下结论\n")
worst = defaultdict(float)
for key in rows:
    worst[key[4]] = max(worst[key[4]], spread(key, "msgs_per_s"))
print("| " + " | ".join(worst) + " |")
print("|" + "---|" * len(worst))
print("| " + " | ".join(f"最大 {v:.0f}%" for v in worst.values()) + " |")
EOF
