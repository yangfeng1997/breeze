// Agent（共享内存 / 仅 UDS）与直连 UDS 的性能对比（设计见 docs/共享内存IPC设计.md §10 / §12）。
//
// 四种被测连接，同一个驱动、同一种消息（64 字节信封 + body，总长 --size）、闭环 ping-pong，每条消息都是"驱动 → 服务端 → 驱动"：
//   uds        普通 UDS 连接：驱动 ⇄ UDS echo 服务端（没有 Agent、没有路由），4 字节长度前缀分帧
//   shm        Agent 的连接方式，只量连接本身：UDS 做握手与探活，数据走两条共享内存环；服务端只做 echo（与 uds 同样最简）
//   agent-shm  完整的 Agent 通路：同 shm，但服务端是 Agent 的路由器；驱动给自己发信封（dst 填自己），
//              路由器解析、核对身份、查表、改写头，把它写回驱动的 Rx 环
//   agent-uds  完整的 Agent 通路，但协商成仅 UDS（AgentClient transport = kUdsOnly），数据走 UDS
// 读法：shm 对 uds 是"UDS+共享内存连接 vs 普通 UDS 连接"的纯传输对比；agent-shm 对 shm 是路由器的开销；
// agent-shm 对 agent-uds 是同一个 Agent 服务端下的传输方式对照。
// 每个角色一个进程（子命令 agent / shm-echo / uds-echo / drive），进程启动时自己绑核，业务回调都在 IO 线程上（不经主循环）。
// 驱动同时保持 --inflight 条请求在途：1 测延迟，大于 1 测闭环吞吐。往返延迟用请求里的 trace_id 带发送时刻、回包时相减。
//
// 用法见 bench/run_agent.sh；单独跑：
//   agent_bench agent     --listen unix:///tmp/a.sock --cpu 2
//   agent_bench shm-echo  --listen unix:///tmp/s.sock --cpu 2
//   agent_bench uds-echo  --listen unix:///tmp/u.sock --cpu 2
//   agent_bench drive     --mode uds|shm|agent-shm|agent-uds --agent|--listen <端点> --size 64 --inflight 1
//                         [--duration 3] [--warmup 1] [--cpu 6] [--watch-pids 123,456]
//                         [--rate R]  定速（只配 inflight 1）：每秒发 R 条，相同负载下比延迟与服务端 CPU；0 = 闭环全速

#include <sched.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "base/net/agent/agent_client.h"
#include "base/net/agent/agent_server.h"
#include "base/net/agent/envelope.h"
#include "base/net/agent/route_table.h"
#include "base/net/codec/length_prefix_codec.h"
#include "base/net/connection/connection_handler_interface.h"
#include "base/net/connection/connection_interface.h"
#include "base/net/endpoint/endpoint.h"
#include "base/net/loop/event_loop_group.h"
#include "base/net/shm/shm_channel_server.h"
#include "base/net/stream/reconnect_policy.h"
#include "base/net/uds/uds_client.h"
#include "base/net/uds/uds_endpoint.h"
#include "base/net/uds/uds_server.h"

using namespace breeze;
using namespace breeze::agent;
using namespace std::chrono_literals;

namespace
{

constexpr NodeId kDriverNode = 1001;

std::uint64_t NowNs()
{
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
}

void Pin(int cpu)
{
    if (cpu < 0)
        return;
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(cpu, &s);
    if (sched_setaffinity(0, sizeof(s), &s) != 0)
        std::perror("sched_setaffinity");
}

[[noreturn]] void Die(const std::string& msg)
{
    std::fprintf(stderr, "agent_bench: %s\n", msg.c_str());
    std::exit(2);
}

// 所有角色共用：积压水位放大（闭环在途 K × size 可能超过默认的 256 KiB，不能让对照被"慢消费者关闭"搅了）。
ConnectionOptions BenchConnection()
{
    ConnectionOptions c;
    c.high_water_mark = 256ull << 20;
    return c;
}

struct Args
{
    std::string cmd;
    std::map<std::string, std::string> kv;
    std::string Get(const std::string& k, const std::string& def = "") const
    {
        const auto it = kv.find(k);
        return it == kv.end() ? def : it->second;
    }
    long GetInt(const std::string& k, long def) const
    {
        const auto it = kv.find(k);
        return it == kv.end() ? def : std::strtol(it->second.c_str(), nullptr, 10);
    }
};

Args ParseArgs(int argc, char** argv)
{
    Args a;
    if (argc < 2)
        Die("缺少子命令（agent | shm-echo | uds-echo | drive）");
    a.cmd = argv[1];
    for (int i = 2; i + 1 < argc; i += 2)
    {
        if (std::strncmp(argv[i], "--", 2) != 0)
            Die(std::string("参数格式不对: ") + argv[i]);
        a.kv[argv[i] + 2] = argv[i + 1];
    }
    return a;
}

// 读进程累计的 CPU 时间（utime + stime，单位 clock tick）。进程名里可能有空格与括号：从最后一个 ')' 之后切。
std::optional<std::uint64_t> CpuTicks(long pid)
{
    char path[64];
    std::snprintf(path, sizeof(path), pid == 0 ? "/proc/self/stat" : "/proc/%ld/stat", pid);
    std::FILE* f = std::fopen(path, "r");
    if (f == nullptr)
        return std::nullopt;
    char buf[1024] = {0};
    const std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[n] = 0;
    const char* p = std::strrchr(buf, ')');
    if (p == nullptr)
        return std::nullopt;
    unsigned long long ut = 0, st = 0;
    // ')' 之后依次是 state ppid pgrp session tty_nr tpgid flags minflt cminflt majflt cmajflt utime stime
    if (std::sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) != 2)
        return std::nullopt;
    return ut + st;
}

// ---- 普通 UDS 的 echo 服务端：收到什么就回什么，只把信封的 src / dst 对调、type 改成 Response ----
class EchoHandler final : public IConnectionHandler
{
public:
    void OnMessage(IConnection& c, std::span<const std::byte> m) noexcept override
    {
        if (m.size() < kEnvelopeHeadSize)
            return;
        buf_.resize(m.size());
        std::memcpy(buf_.data(), m.data(), m.size());
        EnvelopeHead h;
        std::memcpy(&h, buf_.data(), sizeof(h));
        std::swap(h.src, h.dst);
        h.type = static_cast<std::uint8_t>(EnvType::kResponse);
        h.hops = 0;
        WriteEnvelopeHead(buf_, h);
        (void)c.Send(std::span<const std::byte>(buf_.data(), buf_.size()));
    }

private:
    std::vector<std::byte> buf_;
};

// ---- 共享内存 echo 服务端（只量连接本身，不做路由）：把 Tx 环里的记录原样写进 Rx 环，只改信封的 src / dst / type，与 EchoHandler 同一份逻辑 ----
class ShmEchoHandler final : public ShmChannelHandler
{
public:
    // 驱动退出（UDS 断开）时把两条环的唤醒统计打出来。控制块在共享内存里，两端各写各的那一半，服务端能同时看到驱动那一侧：
    // tx 环：驱动是生产者、服务端是消费者；rx 环：服务端是生产者、驱动是消费者。计数从连接建立起累计（含预热）。
    void OnDetached(ShmServerChannel& c, std::chrono::milliseconds) noexcept override
    {
        const auto ull = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
        shm::RingControl& tx = c.shm().tx_control();
        shm::RingControl& rx = c.shm().rx_control();
        std::printf("WAKE tx_msgs=%llu tx_sent=%llu tx_woken=%llu tx_spurious=%llu tx_empty=%llu "
                    "rx_msgs=%llu rx_sent=%llu rx_woken=%llu rx_spurious=%llu rx_empty=%llu\n",
                    ull(tx.consumer_stats.records_popped.Get()),
                    ull(tx.producer_stats.wakeups_sent.Get()),
                    ull(tx.consumer_stats.wakeups_received.Get()),
                    ull(tx.consumer_stats.spurious_wakeups.Get()),
                    ull(tx.consumer_stats.empty_wakeups.Get()),
                    ull(rx.consumer_stats.records_popped.Get()),
                    ull(rx.producer_stats.wakeups_sent.Get()),
                    ull(rx.consumer_stats.wakeups_received.Get()),
                    ull(rx.consumer_stats.spurious_wakeups.Get()),
                    ull(rx.consumer_stats.empty_wakeups.Get()));
        std::fflush(stdout);
    }

    bool OnReadable(ShmServerChannel& c) noexcept override
    {
        for (int left = 64; left > 0; --left) // 与 App 侧的读预算同量级：用完让出，库会再叫一次
        {
            shm::RecordView v;
            const shm::PeekStatus st = c.tx().Peek(v);
            if (st == shm::PeekStatus::kEmpty)
                return false;
            if (st == shm::PeekStatus::kCorrupt)
            {
                c.Close();
                return false;
            }
            auto r = c.rx().TryReserve(v.len);
            if (r.status == shm::ReserveStatus::kFull)
                return true; // Rx 满了：不 Pop，让出，下一轮再试
            if (r.status != shm::ReserveStatus::kOk)
            {
                c.tx().Pop(); // 超过单条上限等：丢掉
                continue;
            }
            std::memcpy(r.payload.data(), v.payload, v.len);
            if (v.len >= kEnvelopeHeadSize)
            {
                EnvelopeHead h;
                std::memcpy(&h, r.payload.data(), sizeof(h));
                std::swap(h.src, h.dst);
                h.type = static_cast<std::uint8_t>(EnvType::kResponse);
                h.hops = 0;
                std::memcpy(r.payload.data(), &h, sizeof(h));
            }
            c.rx().Commit();
            c.tx().Pop();
        }
        return true;
    }
};

// ---- 驱动：保持 inflight 条请求在途，回包一到就发下一条；预热之后的回包计入统计 ----
class DriveHandler final : public IConnectionHandler
{
public:
    // stride：每 stride 条回包取 1 条做延迟样本（吞吐用总数，不受影响；高吞吐时样本数组装不下全部）。
    // rate > 0：定速（只用于 inflight = 1）：每秒发 rate 条，回包早到就自旋等到下一个发送时刻；0 = 闭环全速。
    DriveHandler(std::size_t size, int inflight, bool with_route, std::uint32_t stride, std::uint64_t rate)
        : size_(size)
        , inflight_(inflight)
        , stride_(stride == 0 ? 1 : stride)
        , interval_ns_(rate == 0 ? 0 : 1'000'000'000ull / rate)
    {
        msg_.assign(size, std::byte{0x5a});
        EnvelopeHead h{};
        h.magic = kEnvelopeMagic;
        h.version = kEnvelopeVersion;
        h.type = static_cast<std::uint8_t>(EnvType::kRequest);
        h.body_len = static_cast<std::uint32_t>(size - kEnvelopeHeadSize);
        h.msg_id = 1;
        h.src = kDriverNode;
        h.dst = with_route ? kDriverNode : 0; // 发给自己：Agent 的路由器把它原路转回来
        h.route_type = static_cast<std::uint8_t>(RouteType::kDirect);
        h.remaining_ms = 60000; // 比整个压测都长：不让"排队过期"混进结果
        WriteEnvelopeHead(msg_, h);
        samples_.resize(kMaxSamples);
    }

    void OnConnected(IConnection& c) noexcept override
    {
        connected.store(true, std::memory_order_release);
        last_reply_ns.store(NowNs(), std::memory_order_relaxed);
        for (int i = 0; i < inflight_; ++i)
            SendOne(c);
    }

    void OnMessage(IConnection& c, std::span<const std::byte> m) noexcept override
    {
        const std::uint64_t now = NowNs();
        std::uint32_t err = 0;
        if (m.size() != size_ || (std::memcpy(&err, m.data() + 44, 4), err != 0))
        {
            errors.fetch_add(1, std::memory_order_relaxed);
            return; // 这一条没了，在途数少一个；STALL 看门狗兜底
        }
        last_reply_ns.store(now, std::memory_order_relaxed);
        if (measuring.load(std::memory_order_acquire))
        {
            const std::uint64_t n = total_.fetch_add(1, std::memory_order_relaxed);
            if (n % stride_ == 0)
            {
                std::uint64_t sent = 0;
                std::memcpy(&sent, m.data() + 56, 8); // trace_id：发送时刻
                const std::uint64_t rtt = now - sent;
                const std::size_t i = count_.load(std::memory_order_relaxed);
                if (i < kMaxSamples)
                {
                    samples_[i] = rtt > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(rtt);
                    count_.store(i + 1, std::memory_order_release);
                }
            }
        }
        if (stop.load(std::memory_order_relaxed))
            return;
        if (interval_ns_ != 0) // 定速：这条回包已经处理完、时间戳已取，自旋等到下一个发送时刻再发（只影响驱动自己的核）
        {
            next_send_ns_ = std::max(next_send_ns_ + interval_ns_, now);
            while (NowNs() < next_send_ns_)
                __builtin_ia32_pause();
        }
        SendOne(c);
    }

    void OnClosed(IConnection&, CloseReason, int) noexcept override { closed.store(true, std::memory_order_release); }

    std::uint64_t Total() const { return total_.load(std::memory_order_acquire); }
    std::vector<std::uint32_t> TakeSamples()
    {
        samples_.resize(count_.load(std::memory_order_acquire));
        return std::move(samples_);
    }

    std::atomic<bool> connected{false};
    std::atomic<bool> closed{false};
    std::atomic<bool> measuring{false};
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> errors{0};
    std::atomic<std::uint64_t> last_reply_ns{0};

private:
    static constexpr std::size_t kMaxSamples = 16u << 20;

    void SendOne(IConnection& c)
    {
        const std::uint64_t seq = ++seq_;
        const std::uint64_t now = NowNs();
        std::memcpy(msg_.data() + 48, &seq, 8);
        std::memcpy(msg_.data() + 56, &now, 8);
        if (c.Send(std::span<const std::byte>(msg_.data(), msg_.size())) != SendResult::Ok)
            errors.fetch_add(1, std::memory_order_relaxed);
    }

    const std::size_t size_;
    const int inflight_;
    const std::uint32_t stride_;
    const std::uint64_t interval_ns_;
    std::uint64_t next_send_ns_ = 0;
    std::atomic<std::uint64_t> total_{0};
    std::vector<std::byte> msg_;
    std::uint64_t seq_ = 0;
    std::vector<std::uint32_t> samples_;
    std::atomic<std::size_t> count_{0};
};

void WaitForSignal()
{
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGTERM);
    sigaddset(&s, SIGINT);
    int sig = 0;
    sigwait(&s, &sig);
}

template <typename Pred>
bool WaitUntil(Pred p, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!p())
    {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

std::uint64_t Percentile(const std::vector<std::uint32_t>& sorted, double q)
{
    if (sorted.empty())
        return 0;
    const std::size_t i = std::min(sorted.size() - 1, static_cast<std::size_t>(q * static_cast<double>(sorted.size())));
    return sorted[i];
}

// ---- 子命令 ----

int RunAgent(const Args& a)
{
    const auto ep = Endpoint::Parse(a.Get("listen"));
    if (!ep)
        Die("--listen 不是合法端点");
    EventLoopGroup group(EventLoopGroupOptions{1, 1024}); // 路由器单线程：必须 1 个 loop
    if (group.Start() != 0)
        Die("EventLoopGroup 启动失败");
    StaticRouteTable table;
    AgentServerOptions o;
    o.connection = BenchConnection();
    AgentServer server(group, *ep, table, std::move(o));
    if (const int e = server.Start(); e != 0)
        Die(std::string("AgentServer 启动失败: ") + std::strerror(e));
    std::printf("READY\n");
    std::fflush(stdout);
    WaitForSignal();
    return 0;
}

AgentClientOptions ClientOptions(const Args& a, NodeId node, const IFrameCodecFactory& codec)
{
    AgentClientOptions o;
    o.busid = std::to_string(node);
    const std::string t = a.Get("transport", "shm");
    o.transport = t == "uds" ? shm::wire::Transport::kUdsOnly : shm::wire::Transport::kShmRequired;
    o.connection = BenchConnection();
    o.codec = &codec;
    o.reconnect = ReconnectPolicy{20ms, 200ms, 2.0, 0.0};
    return o;
}

int RunShmEcho(const Args& a)
{
    const auto ep = Endpoint::Parse(a.Get("listen"));
    if (!ep)
        Die("--listen 不是合法端点");
    ShmEchoHandler handler;
    EventLoopGroup group(EventLoopGroupOptions{1, 16});
    if (group.Start() != 0)
        Die("EventLoopGroup 启动失败");
    ShmChannelServer server(group, *ep, ShmServerOptions{}, handler);
    if (const int e = server.Start(); e != 0)
        Die(std::string("ShmChannelServer 启动失败: ") + std::strerror(e));
    std::printf("READY\n");
    std::fflush(stdout);
    WaitForSignal();
    return 0;
}

int RunUdsEcho(const Args& a)
{
    const auto ep = UdsEndpoint::Parse(a.Get("listen"));
    if (!ep)
        Die("--listen 不是合法的 UDS 端点");
    LengthPrefixCodecFactory codec(1 << 20);
    EchoHandler handler;
    EventLoopGroup group(EventLoopGroupOptions{1, 16});
    if (group.Start() != 0)
        Die("EventLoopGroup 启动失败");
    UdsServerOptions o;
    o.connection = BenchConnection();
    UdsServer server(group, *ep, std::move(o), handler, codec);
    if (const int e = server.Start(); e != 0)
        Die(std::string("UdsServer 启动失败: ") + std::strerror(e));
    std::printf("READY\n");
    std::fflush(stdout);
    WaitForSignal();
    return 0;
}

int RunDrive(const Args& a)
{
    const std::string mode = a.Get("mode");
    const std::size_t size = static_cast<std::size_t>(a.GetInt("size", 64));
    const int inflight = static_cast<int>(a.GetInt("inflight", 1));
    const auto duration = std::chrono::milliseconds(a.GetInt("duration", 3) * 1000);
    const auto warmup = std::chrono::milliseconds(a.GetInt("warmup", 1) * 1000);
    const long rate = a.GetInt("rate", 0);
    if (rate > 0 && inflight != 1)
        Die("--rate 只能配 --inflight 1");
    if (size < kEnvelopeHeadSize)
        Die("--size 不能小于信封头 64 字节");
    if (mode != "uds" && mode != "shm" && mode != "agent-uds" && mode != "agent-shm")
        Die("--mode 只能是 uds | shm | agent-shm | agent-uds");
    std::vector<long> watch;
    for (std::size_t pos = 0, next; pos < a.Get("watch-pids").size(); pos = next + 1)
    {
        const std::string s = a.Get("watch-pids");
        next = s.find(',', pos);
        if (next == std::string::npos)
            next = s.size();
        if (next > pos)
            watch.push_back(std::strtol(s.substr(pos, next - pos).c_str(), nullptr, 10));
    }

    LengthPrefixCodecFactory codec(1 << 20);
    DriveHandler handler(size, inflight, mode.rfind("agent", 0) == 0, // 只有走路由器的才需要 dst 填自己
                         inflight > 1 ? 16 : 1, static_cast<std::uint64_t>(rate));
    EventLoopGroup group(EventLoopGroupOptions{1, 16});
    if (group.Start() != 0)
        Die("EventLoopGroup 启动失败");
    std::unique_ptr<INetClient> client;
    if (mode == "uds")
    {
        const auto ep = UdsEndpoint::Parse(a.Get("listen"));
        if (!ep)
            Die("--listen 不是合法的 UDS 端点");
        UdsClientOptions o;
        o.connection = BenchConnection();
        client = std::make_unique<UdsClient>(group, *ep, std::move(o), ReconnectPolicy{20ms, 200ms, 2.0, 0.0}, handler, codec);
    }
    else
    {
        const auto ep = Endpoint::Parse(a.Get("agent"));
        if (!ep)
            Die("--agent 不是合法端点");
        Args t = a;
        t.kv["transport"] = mode == "agent-uds" ? "uds" : "shm"; // shm 与 agent-shm 都是共享内存
        client = std::make_unique<AgentClient>(group, *ep, ClientOptions(t, kDriverNode, codec), handler);
    }
    if (client->Start() != 0)
        Die("客户端启动失败");
    if (!WaitUntil([&] { return handler.connected.load(std::memory_order_acquire); }, 10s))
        Die("驱动连不上");

    std::this_thread::sleep_for(warmup);
    std::vector<std::optional<std::uint64_t>> t0;
    for (const long pid : watch)
        t0.push_back(CpuTicks(pid));
    const auto self0 = CpuTicks(0);
    const auto wall0 = std::chrono::steady_clock::now();
    handler.measuring.store(true, std::memory_order_release);

    // 测量窗口内每 50ms 看一眼有没有卡住（2 秒没有任何回包就是丢了请求，闭环走不下去）。
    while (std::chrono::steady_clock::now() - wall0 < duration)
    {
        std::this_thread::sleep_for(50ms);
        if (handler.closed.load(std::memory_order_acquire))
            Die("STALL: 连接被关闭");
        if (NowNs() - handler.last_reply_ns.load(std::memory_order_relaxed) > 2'000'000'000ull)
            Die("STALL: 2 秒没有回包");
    }
    handler.measuring.store(false, std::memory_order_release);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
    std::vector<std::optional<std::uint64_t>> t1;
    for (const long pid : watch)
        t1.push_back(CpuTicks(pid));
    const auto self1 = CpuTicks(0);
    handler.stop.store(true, std::memory_order_relaxed);
    std::this_thread::sleep_for(100ms); // 让 IO 线程里正在写的那一条落完
    const std::uint64_t total = handler.Total();

    std::vector<std::uint32_t> samples = handler.TakeSamples();
    std::sort(samples.begin(), samples.end());
    const double tick = static_cast<double>(sysconf(_SC_CLK_TCK));
    const auto pct = [&](const std::optional<std::uint64_t>& b, const std::optional<std::uint64_t>& e)
    { return (b && e) ? 100.0 * (static_cast<double>(*e - *b) / tick) / secs : -1.0; };

    const double rps = static_cast<double>(total) / secs; // 吞吐用总数；samples 只是延迟的抽样
    std::printf("RESULT mode=%s size=%zu inflight=%d rate=%ld round_trips_per_s=%.0f mb_per_s=%.1f p50_us=%.1f p90_us=%.1f p99_us=%.1f "
                "p999_us=%.1f max_us=%.1f errors=%llu drv_cpu_pct=%.1f",
                mode.c_str(),
                size,
                inflight,
                rate,
                rps,
                rps * static_cast<double>(size) / 1e6,
                Percentile(samples, 0.50) / 1e3,
                Percentile(samples, 0.90) / 1e3,
                Percentile(samples, 0.99) / 1e3,
                Percentile(samples, 0.999) / 1e3,
                (samples.empty() ? 0.0 : samples.back() / 1e3),
                static_cast<unsigned long long>(handler.errors.load()),
                pct(self0, self1));
    for (std::size_t i = 0; i < watch.size(); ++i)
        std::printf(" watch%zu_cpu_pct=%.1f", i, pct(t0[i], t1[i]));
    std::printf("\n");
    std::fflush(stdout);
    std::_Exit(0); // 对端进程马上被脚本杀掉，不做优雅关闭
}

} // namespace

int main(int argc, char** argv)
{
    const Args a = ParseArgs(argc, argv);
    // 先挡住 SIGTERM / SIGINT：之后创建的线程都继承这个掩码，只有 main 里的 sigwait 收信号。
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGTERM);
    sigaddset(&s, SIGINT);
    pthread_sigmask(SIG_BLOCK, &s, nullptr);
    Pin(static_cast<int>(a.GetInt("cpu", -1)));
    if (a.cmd == "agent")
        return RunAgent(a);
    if (a.cmd == "shm-echo")
        return RunShmEcho(a);
    if (a.cmd == "uds-echo")
        return RunUdsEcho(a);
    if (a.cmd == "drive")
        return RunDrive(a);
    Die("未知子命令: " + a.cmd);
}
