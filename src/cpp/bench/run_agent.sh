#!/bin/bash
# Agent 的连接（UDS 握手 + 共享内存数据通道）与普通 UDS 连接的性能对比：延迟与闭环吞吐。
#   uds        普通 UDS 连接：驱动 ⇄ UDS echo 服务端
#   shm        Agent 的连接方式，只量连接本身：服务端只做 echo，数据走共享内存
#   agent-shm  完整的 Agent 通路：服务端是路由器（解析信封、查表、改写头再投递），数据走共享内存
#   agent-uds  完整的 Agent 通路，但协商成仅 UDS
# 读法：shm 对 uds 是纯传输对比；agent-shm 对 shm 是路由器的开销；agent-shm 对 agent-uds 是同一个 Agent 下传输方式的对照。
# 每个角色一个进程，进程自己绑核：服务端 cpu2、驱动 cpu6（物理核不同，各自的 SMT 兄弟核留空）；方案按 i9-14900KF 定的，换机器请改。
# 消息大小含 64 字节信封（--size 64 就是只有头）。inflight = 驱动同时保持几条请求在途：1 测延迟，大于 1 测闭环吞吐。
# 闭环全速时延迟只是排队、两边 CPU 都跑满，比不出效率，所以另有一组"定速"（RATES，单条在途、每秒 R 次往返）：负载相同，才好比延迟与服务端 CPU。
# 重复在最外层、模式在内层交错，让温度与后台噪声对各模式的影响大致相同；每组取中位数。
#
# 用法：[SIZES="64 1024 16384"] [INFLIGHTS="1 64"] [RATES="5000 20000"] [RATE_SIZES="1024"] [MODES="uds shm agent-shm agent-uds"] bench/run_agent.sh [agent_bench 路径] [重复次数] [每组计时秒数]
#   SUMMARIZE_ONLY=1 RAW=<已有的原始结果文件> bench/run_agent.sh   只重新汇总，不跑
#   cmake --preset release -DBREEZE_BUILD_AGENT_BENCH=ON && cmake --build ../../build/cpp-release --target agent_bench

set -u
BIN=${1:-../../build/cpp-release/agent_bench}
REPS=${2:-3}
DURATION=${3:-3}
SERVER_CPU=2
DRIVER_CPU=6
SIZES=${SIZES:-"64 1024 16384"}
INFLIGHTS=${INFLIGHTS:-"1 64"}
MODES=${MODES:-"uds shm agent-shm agent-uds"}
RATES=${RATES:-"5000 20000"}
RATE_SIZES=${RATE_SIZES:-"1024"}
RAW=${RAW:-/tmp/agent_bench_raw.txt}

server_cmd_for() {
  case "$1" in
    uds) echo uds-echo ;;
    shm) echo shm-echo ;;
    agent-shm | agent-uds) echo agent ;;
  esac
}

run_one() { # rep mode size inflight [rate]
  local rep=$1 mode=$2 size=$3 inflight=$4 rate=${5:-0}
  local dir
  dir=$(mktemp -d /tmp/agent_bench.XXXXXX)
  "$BIN" "$(server_cmd_for "$mode")" --listen "unix://$dir/s.sock" --cpu "$SERVER_CPU" >"$dir/srv.log" 2>&1 &
  local srv=$!
  local i
  for i in $(seq 1 200); do
    grep -q READY "$dir/srv.log" 2>/dev/null && break
    sleep 0.05
  done
  if ! grep -q READY "$dir/srv.log" 2>/dev/null; then
    echo "服务端没起来: $mode" >&2
    cat "$dir/srv.log" >&2
    kill "$srv" 2>/dev/null
    rm -rf "$dir"
    return 1
  fi
  local endpoint_flag=--agent
  [ "$mode" = uds ] && endpoint_flag=--listen
  local out
  out=$("$BIN" drive --mode "$mode" "$endpoint_flag" "unix://$dir/s.sock" --size "$size" --inflight "$inflight" \
    --duration "$DURATION" --warmup 1 --cpu "$DRIVER_CPU" --rate "$rate" --watch-pids "$srv" 2>&1)
  local rc=$?
  if [ "$mode" = shm ]; then # shm echo 服务端在驱动断开时打印唤醒统计（WAKE 行）；等一下让它打完
    sleep 0.3
    grep -h '^WAKE' "$dir/srv.log" | sed "s/^WAKE/WAKE rep=$rep mode=$mode size=$size inflight=$inflight rate=$rate/" >>"$RAW.wake"
  fi
  kill "$srv" 2>/dev/null
  wait "$srv" 2>/dev/null
  rm -rf "$dir"
  if [ $rc -ne 0 ] || ! grep -q '^RESULT' <<<"$out"; then
    echo "驱动失败(rc=$rc): mode=$mode size=$size inflight=$inflight rate=$rate: $out" >&2
    return 1
  fi
  grep '^RESULT' <<<"$out" | sed "s/^RESULT/RESULT rep=$rep/" >>"$RAW"
}

run_all() {
  : >"$RAW"
  : >"$RAW.wake"
  local rep size inflight mode rate
  for rep in $(seq 1 "$REPS"); do
    for inflight in $INFLIGHTS; do
      for size in $SIZES; do
        for mode in $MODES; do
          run_one "$rep" "$mode" "$size" "$inflight" || true
        done
      done
    done
    for rate in $RATES; do
      for size in $RATE_SIZES; do
        for mode in $MODES; do
          run_one "$rep" "$mode" "$size" 1 "$rate" || true
        done
      done
    done
  done
}

summarize() {
  RAW="$RAW" MODES="$MODES" SIZES="$SIZES" INFLIGHTS="$INFLIGHTS" RATES="$RATES" RATE_SIZES="$RATE_SIZES" python3 - <<'EOF'
import os, re, statistics, collections
rows = collections.defaultdict(list)
for line in open(os.environ["RAW"]):
    if not line.startswith("RESULT"):
        continue
    kv = dict(p.split("=", 1) for p in line.split()[1:])
    rows[(kv["mode"], int(kv["size"]), int(kv["inflight"]), int(kv.get("rate", 0)))].append({k: float(v) for k, v in kv.items() if k not in ("mode", "rep")})
modes = os.environ["MODES"].split()
sizes = [int(x) for x in os.environ["SIZES"].split()]
inflights = [int(x) for x in os.environ["INFLIGHTS"].split()]
rates = [int(x) for x in os.environ["RATES"].split()]
rate_sizes = [int(x) for x in os.environ["RATE_SIZES"].split()]
def med(mode, size, inflight, key, rate=0):
    r = rows.get((mode, size, inflight, rate))
    return statistics.median(x[key] for x in r) if r else None
def n(mode, size, inflight, rate=0):
    return len(rows.get((mode, size, inflight, rate), []))
def fmt(v, f="{:.1f}"):
    return "-" if v is None else f.format(v)

for inflight in inflights:
    kind = "延迟（单条在途）" if inflight == 1 else f"闭环吞吐（{inflight} 条在途）"
    print(f"\n### {kind}\n")
    print("| 消息大小 | 模式 | 往返/秒 | MB/s | p50 µs | p90 µs | p99 µs | p99.9 µs | 服务端 CPU% | 驱动 CPU% | 轮数 |")
    print("|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for size in sizes:
        for mode in modes:
            if n(mode, size, inflight) == 0:
                continue
            print(f"| {size} | {mode} | {fmt(med(mode,size,inflight,'round_trips_per_s'),'{:,.0f}')} | {fmt(med(mode,size,inflight,'mb_per_s'))} | "
                  f"{fmt(med(mode,size,inflight,'p50_us'))} | {fmt(med(mode,size,inflight,'p90_us'))} | {fmt(med(mode,size,inflight,'p99_us'))} | "
                  f"{fmt(med(mode,size,inflight,'p999_us'))} | {fmt(med(mode,size,inflight,'watch0_cpu_pct'),'{:.0f}')} | "
                  f"{fmt(med(mode,size,inflight,'drv_cpu_pct'),'{:.0f}')} | {n(mode,size,inflight)} |")

print("\n### 定速负载（单条在途，每秒 R 次往返）：相同负载下比延迟与服务端 CPU\n")
print("| 消息大小 | 目标 R | 模式 | 实际往返/秒 | p50 µs | p90 µs | p99 µs | p99.9 µs | 服务端 CPU% | 轮数 |")
print("|---:|---:|---|---:|---:|---:|---:|---:|---:|---:|")
for size in rate_sizes:
    for rate in rates:
        for mode in modes:
            if n(mode, size, 1, rate) == 0:
                continue
            print(f"| {size} | {rate:,} | {mode} | {fmt(med(mode,size,1,'round_trips_per_s',rate),'{:,.0f}')} | {fmt(med(mode,size,1,'p50_us',rate))} | "
                  f"{fmt(med(mode,size,1,'p90_us',rate))} | {fmt(med(mode,size,1,'p99_us',rate))} | {fmt(med(mode,size,1,'p999_us',rate))} | "
                  f"{fmt(med(mode,size,1,'watch0_cpu_pct',rate),'{:.1f}')} | {n(mode,size,1,rate)} |")

wake = collections.defaultdict(list)
if os.path.exists(os.environ["RAW"] + ".wake"):
    for line in open(os.environ["RAW"] + ".wake"):
        if line.startswith("WAKE"):
            kv = dict(p.split("=", 1) for p in line.split()[1:])
            wake[(int(kv["size"]), int(kv["inflight"]), int(kv["rate"]))].append({k: float(v) for k, v in kv.items() if k not in ("mode", "rep")})
if wake:
    print("\n### 共享内存连接的唤醒统计（shm 模式，整条连接累计，含预热；中位数）\n")
    print("`sent` 是生产者敲 eventfd 的次数，`woken` 是消费者醒来时读到计数的次数，`empty` 是其中第一次 Peek 就是空的（旧计数，白醒）。\n")
    print("| 在途 | 目标 R | 消息大小 | 方向 | 消息数 | 每条敲 eventfd 次数 | 每条睡醒次数 | 空唤醒占醒来的比例 | 虚假唤醒（eventfd 空） |")
    print("|---:|---:|---:|---|---:|---:|---:|---:|---:|")
    for (size, inflight, rate), rs in sorted(wake.items(), key=lambda x: (x[0][1], x[0][2], x[0][0])):
        for d, name in (("tx", "驱动→服务端"), ("rx", "服务端→驱动")):
            g = lambda k: statistics.median(r[f"{d}_{k}"] for r in rs)
            msgs = g("msgs")
            if msgs <= 0:
                continue
            woken = g("woken")
            print(f"| {inflight} | {rate if rate else '全速'} | {size} | {name} | {msgs:,.0f} | {g('sent')/msgs:.3f} | {woken/msgs:.3f} | "
                  f"{(g('empty')/woken*100 if woken else 0):.1f}% | {g('spurious'):,.0f} |")

print("\n### 共享内存连接相对普通 UDS 连接（shm / uds，中位数之比）\n")
print("| 在途 | 消息大小 | 吞吐比 | p50 延迟比 | p99 延迟比 | 服务端 CPU 比 |")
print("|---:|---:|---:|---:|---:|---:|")
for inflight in inflights:
    for size in sizes:
        def ratio(key):
            a, b = med("shm", size, inflight, key), med("uds", size, inflight, key)
            return "-" if not a or not b else f"{a / b:.2f}×"
        if n("shm", size, inflight) and n("uds", size, inflight):
            print(f"| {inflight} | {size} | {ratio('round_trips_per_s')} | {ratio('p50_us')} | {ratio('p99_us')} | {ratio('watch0_cpu_pct')} |")
EOF
}

if [ "${SUMMARIZE_ONLY:-0}" != 1 ]; then
  run_all
fi
echo "环境：$(lscpu | grep -m1 'Model name' | sed 's/.*: *//')，$(uname -r)；服务端 cpu$SERVER_CPU、驱动 cpu$DRIVER_CPU；每组 ${DURATION}s（预热 1s），重复 ${REPS} 次取中位数"
summarize
