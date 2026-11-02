// 共享内存环 vs 进程内无锁队列：同一台机器上两个线程 / 两个进程之间传消息有多快（设计见 docs/共享内存IPC设计.md §10）。
//
// 三种实现，同一个测试流程、同一种消息（前 8 字节是序号，总长 --size，按值拷贝进出）：
//   queue        进程内：两个线程，两条 SpscQueue（base/queue，包 moodycamel::ReaderWriterQueue）
//   ring-thread  共享内存环：同一个进程里的两个线程（两条环，一个方向一条）
//   ring-proc    共享内存环：两个进程（fork 之后子进程 Attach 重新映射，与真实的 App / Agent 一样）
// 两端都自旋（不走 eventfd、不睡），各自绑一个物理核（--cpu-a / --cpu-b），只量"队列本身"：
//   两边收到之后都把整段载荷读一遍（异或），免得编译器把队列那边的载荷拷贝优化掉；队列容量取与环同量级的 1 MiB。
//   1) 乒乓：A 发一条、B 原样回一条（B 把收到的拷进返回的队列），A 收到再发下一条；报平均往返（总时间 / 次数，不含逐次读时钟的开销）与抽样的 p50 / p99；
//   2) 单向吞吐：A 连续发 N 条，B 只收，最后 B 回一条确认；--batch K 时环一次发布 K 条（队列没有批量发布）。
// 结论怎么读：环与队列的差是"设计"（拷进环 + 记录头与校验 + 对端不可信的防御）；ring-thread 与 ring-proc 的差才是"拆成两个进程"的代价。
//
// 用法：ring_bench --impl queue|ring-thread|ring-proc --size 64 [--iters 200000] [--stream 2000000] [--batch 1]
//                   [--cpu-a 2] [--cpu-b 4]
// 输出一行 RESULT，由 bench/run_ring.sh 汇总。

#include <pthread.h>
#include <sched.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "base/queue/spsc_queue.h"
#include "base/shm/shm_channel.h"
#include "base/shm/shm_fd.h"
#include "base/shm/shm_ring.h"

using namespace breeze;
using Clock = std::chrono::steady_clock;

namespace
{

constexpr std::uint64_t kEndPing = ~0ULL - 1; // 乒乓结束标记
constexpr std::uint64_t kEndStream = ~0ULL - 2; // 单向流结束标记
constexpr std::uint64_t kAck = ~0ULL - 3;

[[noreturn]] void Die(const std::string& m)
{
    std::fprintf(stderr, "ring_bench: %s\n", m.c_str());
    std::_Exit(2);
}

void PinThis(int cpu)
{
    if (cpu < 0)
        return;
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(cpu, &s);
    if (pthread_setaffinity_np(pthread_self(), sizeof(s), &s) != 0)
        std::perror("pthread_setaffinity_np");
}

inline void Pause() { __builtin_ia32_pause(); }

// 收到一条之后"用一下"载荷：把每个 8 字节字异或起来，结果交给一个编译器看不透的汇编。
// 不这样做的话，队列那边（按值拷贝出来、只读序号）会被编译器把整段载荷的拷贝优化掉，对比对环不公平。
inline void Touch(const void* p, std::size_t n)
{
    std::uint64_t acc = 0;
    const auto* b = static_cast<const unsigned char*>(p);
    for (std::size_t i = 0; i + 8 <= n; i += 8)
    {
        std::uint64_t w;
        std::memcpy(&w, b + i, 8);
        acc ^= w;
    }
    asm volatile("" : : "r"(acc) : "memory");
}

// 自旋等待的看门狗：10 秒还没动静就是对端挂了 / 协议错了，直接退出而不是挂死。
struct Watchdog
{
    std::uint64_t n = 0;
    Clock::time_point start = Clock::now();
    void Tick()
    {
        if ((++n & 0xFFFFF) == 0 && Clock::now() - start > std::chrono::seconds(10))
            Die("自旋超过 10 秒没有进展（对端挂了？）");
    }
    void Reset() { n = 0; start = Clock::now(); }
};

// ---- 链路：A、B 两端用同一套接口 ----
// Send(seq, flush)：自旋直到发出去；Flush()：把攒着的批量发布出去；TryRecv(seq)：收一条并消费；TryEcho(seq)：收一条并原样回一条。

template <std::size_t N>
struct QueueMsg
{
    std::uint64_t seq;
    std::array<std::byte, N - 8> pad;
};

template <std::size_t N>
class QueueLink
{
public:
    QueueLink(SpscQueue<QueueMsg<N>>& tx, SpscQueue<QueueMsg<N>>& rx)
        : tx_(tx)
        , rx_(rx)
    {
        snd_.pad.fill(std::byte{0x5a}); // 载荷是真的有内容的，不是未初始化（否则拷贝可能被优化掉）
    }
    void Send(std::uint64_t seq, bool)
    {
        snd_.seq = seq;
        Watchdog w;
        while (!tx_.TryEnqueue(snd_))
        {
            Pause();
            w.Tick();
        }
    }
    void Flush() {}
    bool TryRecv(std::uint64_t& seq)
    {
        if (!rx_.TryDequeue(rcv_))
            return false;
        seq = rcv_.seq;
        Touch(rcv_.pad.data(), rcv_.pad.size());
        return true;
    }
    bool TryEcho(std::uint64_t& seq)
    {
        if (!rx_.TryDequeue(rcv_)) // 先收进接收缓冲，再从缓冲发回去：与真实的 echo 处理器一样，两次拷贝
            return false;
        seq = rcv_.seq;
        Watchdog w;
        while (!tx_.TryEnqueue(rcv_))
        {
            Pause();
            w.Tick();
        }
        return true;
    }

private:
    SpscQueue<QueueMsg<N>>& tx_;
    SpscQueue<QueueMsg<N>>& rx_;
    QueueMsg<N> snd_{};
    QueueMsg<N> rcv_{};
};

class RingLink
{
public:
    RingLink(shm::RingProducer tx, shm::RingConsumer rx, std::uint32_t size, int batch)
        : tx_(tx)
        , rx_(rx)
        , size_(size)
        , batch_(batch < 1 ? 1 : batch)
        , buf_(size, std::byte{0x5a})
    {
    }
    void Send(std::uint64_t seq, bool flush)
    {
        std::memcpy(buf_.data(), &seq, 8);
        Watchdog w;
        for (;;)
        {
            auto r = tx_.TryReserve(size_);
            if (r.status == shm::ReserveStatus::kOk)
            {
                std::memcpy(r.payload.data(), buf_.data(), size_);
                ++pending_;
                break;
            }
            if (r.status != shm::ReserveStatus::kFull)
                Die("TryReserve 失败（不是 Full）");
            Flush(); // 环满了：先把攒着的发布出去，消费者才能腾空间
            Pause();
            w.Tick();
        }
        if (flush || pending_ >= batch_)
            Flush();
    }
    void Flush()
    {
        if (pending_ > 0)
        {
            tx_.Commit();
            pending_ = 0;
        }
    }
    bool TryRecv(std::uint64_t& seq)
    {
        shm::RecordView v;
        const shm::PeekStatus st = rx_.Peek(v);
        if (st == shm::PeekStatus::kEmpty)
            return false;
        if (st != shm::PeekStatus::kOk)
            Die("Peek 失败（环被写坏？）");
        std::memcpy(&seq, v.payload, 8);
        Touch(v.payload, v.len); // 与队列那边一样把载荷读一遍
        rx_.Pop();
        return true;
    }
    bool TryEcho(std::uint64_t& seq)
    {
        shm::RecordView v;
        const shm::PeekStatus st = rx_.Peek(v);
        if (st == shm::PeekStatus::kEmpty)
            return false;
        if (st != shm::PeekStatus::kOk)
            Die("Peek 失败（环被写坏？）");
        std::memcpy(&seq, v.payload, 8);
        Watchdog w;
        for (;;)
        {
            auto r = tx_.TryReserve(v.len);
            if (r.status == shm::ReserveStatus::kOk)
            {
                std::memcpy(r.payload.data(), v.payload, v.len);
                tx_.Commit();
                break;
            }
            Pause();
            w.Tick();
        }
        rx_.Pop();
        return true;
    }

private:
    shm::RingProducer tx_;
    shm::RingConsumer rx_;
    const std::uint32_t size_;
    const int batch_;
    int pending_ = 0;
    std::vector<std::byte> buf_;
};

// ---- 两个角色 ----

struct Params
{
    std::uint64_t iters = 200000;
    std::uint64_t stream = 2000000;
    std::size_t size = 64;
    int batch = 1;
};

struct Result
{
    double avg_rtt_ns = 0, p50_ns = 0, p99_ns = 0, stream_mmsg_s = 0, stream_gb_s = 0;
};

template <class Link>
void RoleB(Link& l)
{
    std::uint64_t seq = 0;
    Watchdog w;
    for (;;) // 乒乓：原样回
    {
        if (!l.TryEcho(seq))
        {
            Pause();
            w.Tick();
            continue;
        }
        w.Reset();
        if (seq == kEndPing)
            break;
    }
    for (;;) // 单向流：只收
    {
        if (!l.TryRecv(seq))
        {
            Pause();
            w.Tick();
            continue;
        }
        w.Reset();
        if (seq == kEndStream)
            break;
    }
    l.Send(kAck, true);
}

template <class Link>
Result RoleA(Link& l, const Params& p)
{
    Result res;
    std::uint64_t seq = 0;
    Watchdog w;
    const auto pingpong = [&](std::uint64_t n, std::vector<double>* samples)
    {
        for (std::uint64_t i = 0; i < n; ++i)
        {
            const bool sample = samples != nullptr && (i % 16 == 0);
            const auto t0 = sample ? Clock::now() : Clock::time_point{};
            l.Send(i, true);
            w.Reset();
            while (!l.TryRecv(seq))
            {
                Pause();
                w.Tick();
            }
            if (seq != i)
                Die("乒乓序号对不上");
            if (sample)
                samples->push_back(static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count()));
        }
    };
    pingpong(20000, nullptr); // 预热
    std::vector<double> samples;
    samples.reserve(p.iters / 16 + 1);
    const auto t0 = Clock::now();
    pingpong(p.iters, &samples);
    const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    res.avg_rtt_ns = secs * 1e9 / static_cast<double>(p.iters);
    std::sort(samples.begin(), samples.end());
    res.p50_ns = samples[samples.size() / 2];
    res.p99_ns = samples[samples.size() * 99 / 100];
    l.Send(kEndPing, true);
    w.Reset();
    while (!l.TryRecv(seq) || seq != kEndPing)
    {
        Pause();
        w.Tick();
    }

    // 单向吞吐：B 已进入"只收"阶段
    const auto s0 = Clock::now();
    for (std::uint64_t i = 0; i < p.stream; ++i)
        l.Send(i, false);
    l.Send(kEndStream, true);
    w.Reset();
    while (!l.TryRecv(seq) || seq != kAck)
    {
        Pause();
        w.Tick();
    }
    const double ssecs = std::chrono::duration<double>(Clock::now() - s0).count();
    res.stream_mmsg_s = static_cast<double>(p.stream) / ssecs / 1e6;
    res.stream_gb_s = static_cast<double>(p.stream) * static_cast<double>(p.size) / ssecs / 1e9;
    return res;
}

void Print(const char* impl, const Params& p, const Result& r)
{
    std::printf("RESULT impl=%s size=%zu batch=%d avg_rtt_ns=%.1f p50_ns=%.1f p99_ns=%.1f stream_mmsg_s=%.2f stream_gb_s=%.2f\n",
                impl,
                p.size,
                p.batch,
                r.avg_rtt_ns,
                r.p50_ns,
                r.p99_ns,
                r.stream_mmsg_s,
                r.stream_gb_s);
    std::fflush(stdout);
}

template <std::size_t N>
Result RunQueue(const Params& p, int cpu_a, int cpu_b)
{
    constexpr std::size_t kCap = std::max<std::size_t>(64, (1u << 20) / N); // 总字节数与环的 1 MiB 同量级
    SpscQueue<QueueMsg<N>> ab(kCap), ba(kCap);
    std::thread tb(
        [&]
        {
            PinThis(cpu_b);
            QueueLink<N> l(ba, ab); // B：发 ba、收 ab
            RoleB(l);
        });
    PinThis(cpu_a);
    QueueLink<N> l(ab, ba);
    const Result r = RoleA(l, p);
    tb.join();
    return r;
}

Result RunRing(const Params& p, int cpu_a, int cpu_b, bool process)
{
    shm::ChannelOptions o;
    o.tx_capacity = shm::kMinRingCapacity;
    o.rx_capacity = shm::kMinRingCapacity;
    auto ch = shm::ShmChannel::Create(o);
    if (!ch)
        Die("ShmChannel::Create 失败");
    const auto size = static_cast<std::uint32_t>(p.size);
    if (!process)
    {
        std::thread tb(
            [&]
            {
                PinThis(cpu_b);
                RingLink l(ch->RxProducer(), ch->TxConsumer(), size, p.batch); // B：发 rx 环、收 tx 环
                RoleB(l);
            });
        PinThis(cpu_a);
        RingLink l(ch->TxProducer(), ch->RxConsumer(), size, p.batch);
        const Result r = RoleA(l, p);
        tb.join();
        return r;
    }
    const pid_t pid = ::fork();
    if (pid < 0)
        Die("fork 失败");
    if (pid == 0)
    {
        // 子进程：像真实的 Agent 那样把 fd 重新映射一遍（自己的地址空间里是另一个映射）。
        auto mine = shm::ShmChannel::Attach(shm::Fd(::dup(ch->memfd())), shm::Fd(::dup(ch->tx_eventfd())), shm::Fd(::dup(ch->rx_eventfd())), {});
        if (!mine)
            Die("子进程 Attach 失败");
        PinThis(cpu_b);
        RingLink l(mine->RxProducer(), mine->TxConsumer(), size, p.batch);
        RoleB(l);
        std::_Exit(0);
    }
    PinThis(cpu_a);
    RingLink l(ch->TxProducer(), ch->RxConsumer(), size, p.batch);
    const Result r = RoleA(l, p);
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        Die("子进程异常退出");
    return r;
}

} // namespace

int main(int argc, char** argv)
{
    std::map<std::string, std::string> kv;
    for (int i = 1; i + 1 < argc; i += 2)
    {
        if (std::strncmp(argv[i], "--", 2) != 0)
            Die(std::string("参数格式不对: ") + argv[i]);
        kv[argv[i] + 2] = argv[i + 1];
    }
    const auto get = [&](const char* k, long def) { return kv.count(k) ? std::strtol(kv[k].c_str(), nullptr, 10) : def; };
    Params p;
    p.size = static_cast<std::size_t>(get("size", 64));
    p.iters = static_cast<std::uint64_t>(get("iters", 200000));
    p.stream = static_cast<std::uint64_t>(get("stream", 2000000));
    p.batch = static_cast<int>(get("batch", 1));
    const int cpu_a = static_cast<int>(get("cpu-a", 2));
    const int cpu_b = static_cast<int>(get("cpu-b", 4));
    const std::string impl = kv.count("impl") ? kv["impl"] : "";
    Result r;
    if (impl == "queue")
    {
        if (p.size == 64)
            r = RunQueue<64>(p, cpu_a, cpu_b);
        else if (p.size == 1024)
            r = RunQueue<1024>(p, cpu_a, cpu_b);
        else if (p.size == 16384)
            r = RunQueue<16384>(p, cpu_a, cpu_b);
        else
            Die("queue 只支持 --size 64 | 1024 | 16384");
        p.batch = 1;
    }
    else if (impl == "ring-thread" || impl == "ring-proc")
        r = RunRing(p, cpu_a, cpu_b, impl == "ring-proc");
    else
        Die("--impl 只能是 queue | ring-thread | ring-proc");
    Print(impl.c_str(), p, r);
    return 0;
}
