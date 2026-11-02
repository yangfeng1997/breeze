// 环的单元测试：空、满、恰好满、批量发布与释放、记录跨镜像边界、TooLarge、游标绕过 2^64、
// 唤醒协议的各个分支、双线程压力（专门盯"消费者带着数据睡死"）。
// 多进程与崩溃注入在 shm_multiprocess_test，对端写坏数据在 shm_untrusted_test。

#include <poll.h>
#include <sys/eventfd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <random>
#include <thread>
#include <vector>

#include "base/shm/shm_channel.h"
#include "base/shm/shm_test_util.h"

using namespace breeze::shm;
using namespace breeze::shm::testing;

namespace
{

// 读掉 eventfd 里的计数；空则返回 0。
std::uint64_t DrainEventFd(int fd)
{
    std::uint64_t v = 0;
    const ssize_t n = ::read(fd, &v, sizeof(v));
    if (n == static_cast<ssize_t>(sizeof(v)))
        return v;
    CHECK(n < 0 && errno == EAGAIN);
    return 0;
}

void ProduceOne(RingProducer& p, std::uint32_t len, std::uint64_t seq)
{
    auto r = p.TryReserve(len);
    CHECK(r.status == ReserveStatus::kOk);
    CHECK(r.payload.size() == len);
    FillPayload(r.payload, seq);
    p.Commit();
}

void ExpectOne(RingConsumer& c, std::uint32_t len, std::uint64_t seq)
{
    RecordView v;
    CHECK(c.Peek(v) == PeekStatus::kOk);
    CHECK(v.len == len);
    std::uint64_t got = 0;
    CHECK(CheckPayload(v.payload, v.len, &got));
    CHECK(got == seq);
    c.Pop();
}

void TestEmptyAndRoundTrip()
{
    ShmChannel ch = MakeChannel();
    RingProducer p = ch.TxProducer();
    RingConsumer c = ch.TxConsumer();
    CHECK(!p.corrupt() && !c.corrupt());

    RecordView v;
    CHECK(c.Peek(v) == PeekStatus::kEmpty);
    c.Pop(); // 没有成功的 Peek：什么也不做
    CHECK(c.Peek(v) == PeekStatus::kEmpty);

    auto r = p.TryReserve(100);
    CHECK(r.status == ReserveStatus::kOk && r.payload.size() == 100);
    FillPayload(r.payload, 7);
    CHECK(c.Peek(v) == PeekStatus::kEmpty); // Commit 之前对消费者不可见
    CHECK(p.pending_records() == 1);
    p.Commit();
    CHECK(p.pending_records() == 0);

    CHECK(c.Peek(v) == PeekStatus::kOk);
    CHECK(v.len == 100 && v.cursor == 0);
    CHECK(v.enqueue_ts_ns != 0); // 每条都有入队时刻
    std::uint64_t seq = 0;
    CHECK(CheckPayload(v.payload, v.len, &seq) && seq == 7);
    // Peek 不推进：再看还是它。
    RecordView again;
    CHECK(c.Peek(again) == PeekStatus::kOk && again.cursor == 0);
    c.Pop();
    CHECK(c.Peek(v) == PeekStatus::kEmpty);

    // 空 Commit 什么也不做。
    p.Commit();
    CHECK(ch.tx_control().write_idx.load() == RecordSize(100));
    CHECK(ch.tx_control().read_idx.load() == RecordSize(100));

    // 载荷长度 0 也是合法记录，占一个缓存行。
    ProduceOne(p, 8, 1); // FillPayload 要求至少 8 字节，0 字节另测
    ExpectOne(c, 8, 1);
    auto z = p.TryReserve(0);
    CHECK(z.status == ReserveStatus::kOk && z.payload.empty());
    p.Commit();
    CHECK(c.Peek(v) == PeekStatus::kOk && v.len == 0);
    c.Pop();

    const ProducerStats& ps = ch.tx_control().producer_stats;
    const ConsumerStats& cs = ch.tx_control().consumer_stats;
    CHECK(ps.records_committed.Get() == 3);
    CHECK(ps.bytes_committed.Get() == 100 + 8 + 0);
    CHECK(cs.records_popped.Get() == 3);
    CHECK(cs.bytes_popped.Get() == 100 + 8 + 0);
}

void TestBatchPublishAndRelease()
{
    ShmChannel ch = MakeChannel();
    RingProducer p = ch.TxProducer();
    RingConsumer c = ch.TxConsumer();
    RingControl& ctl = ch.tx_control();

    // 批量发布：预留 10 条，一次 Commit，write_idx 只动一次。
    for (std::uint64_t i = 0; i < 10; ++i)
    {
        auto r = p.TryReserve(40);
        CHECK(r.status == ReserveStatus::kOk);
        FillPayload(r.payload, i);
    }
    CHECK(ctl.write_idx.load() == 0);
    p.Commit();
    CHECK(ctl.write_idx.load() == 10 * RecordSize(40));
    CHECK(ctl.producer_stats.records_committed.Get() == 10);

    // 同一批共用一个入队时刻（取批内第一条预留的时刻）。
    RecordView first;
    CHECK(c.Peek(first) == PeekStatus::kOk);
    const std::uint64_t ts = first.enqueue_ts_ns;
    CHECK(ts != 0);

    // 批量释放：只在本地推进，read_idx 不动，直到 Release。
    for (std::uint64_t i = 0; i < 10; ++i)
    {
        RecordView v;
        CHECK(c.Peek(v) == PeekStatus::kOk);
        CHECK(v.enqueue_ts_ns == ts);
        std::uint64_t seq = 0;
        CHECK(CheckPayload(v.payload, v.len, &seq) && seq == i);
        c.PopNoRelease();
    }
    CHECK(ctl.read_idx.load() == 0);
    RecordView v;
    CHECK(c.Peek(v) == PeekStatus::kEmpty);
    c.Release();
    CHECK(ctl.read_idx.load() == 10 * RecordSize(40));

    // 下一批的入队时刻重新取，不沿用上一批（隔 50 us，单调时钟的纳秒读数一定更大）。
    ::usleep(50);
    ProduceOne(p, 40, 99);
    RecordView next;
    CHECK(c.Peek(next) == PeekStatus::kOk);
    CHECK(next.enqueue_ts_ns > ts);
    c.Pop();
}

void TestFullAndExactlyFull()
{
    ShmChannel ch = MakeChannel();
    RingProducer p = ch.TxProducer();
    RingConsumer c = ch.TxConsumer();
    const std::uint32_t kLen = 48; // 16 + 48 = 64：一条记录正好一个缓存行
    const std::uint64_t kCount = kMinRingCapacity / RecordSize(kLen);

    for (std::uint64_t i = 0; i < kCount; ++i)
    {
        auto r = p.TryReserve(kLen);
        CHECK(r.status == ReserveStatus::kOk);
        FillPayload(r.payload, i);
    }
    CHECK(p.TryReserve(kLen).status == ReserveStatus::kFull); // 预留阶段就满了：恰好填满，再来一条放不下
    p.Commit();
    CHECK(p.TryReserve(kLen).status == ReserveStatus::kFull);
    CHECK(ch.tx_control().producer_stats.full_events.Get() >= 2);
    CHECK(ch.tx_control().write_idx.load() - ch.tx_control().read_idx.load() == kMinRingCapacity);

    // 消费一条并释放之后，刚好能再放一条（空间被腾出的瞬间可见）。序号 0 已读走，新放的是序号 kCount。
    ExpectOne(c, kLen, 0);
    auto extra1 = p.TryReserve(kLen);
    CHECK(extra1.status == ReserveStatus::kOk);
    FillPayload(extra1.payload, kCount);
    CHECK(p.TryReserve(kLen).status == ReserveStatus::kFull);
    p.Commit();

    // PopNoRelease 不会让生产者看到空间：序号 1 在本地读走，read_idx 不动，所以仍然是满的。
    RecordView v;
    CHECK(c.Peek(v) == PeekStatus::kOk);
    c.PopNoRelease();
    CHECK(p.TryReserve(kLen).status == ReserveStatus::kFull);
    c.Release();
    auto extra2 = p.TryReserve(kLen);
    CHECK(extra2.status == ReserveStatus::kOk);
    FillPayload(extra2.payload, kCount + 1);
    p.Commit();

    // 剩下的按顺序全部读出来：序号 2 .. kCount + 1，共 kCount 条。
    std::uint64_t next = 2;
    while (c.Peek(v) == PeekStatus::kOk)
    {
        std::uint64_t seq = 0;
        CHECK(CheckPayload(v.payload, v.len, &seq) && seq == next);
        c.Pop();
        ++next;
    }
    CHECK(next == kCount + 2);
}

void TestTooLargeAndMaxPayload()
{
    ShmChannel ch = MakeChannel(); // 1 MiB 的环：载荷上限默认 64 KiB
    RingProducer p = ch.TxProducer();
    RingConsumer c = ch.TxConsumer();
    const std::uint32_t max = ch.layout().tx.max_payload;
    CHECK(max == kMinRingCapacity / 16);
    CHECK(p.TryReserve(max + 1).status == ReserveStatus::kTooLarge);
    CHECK(p.TryReserve(0xFFFFFFFFU).status == ReserveStatus::kTooLarge);
    CHECK(ch.tx_control().producer_stats.full_events.Get() == 0); // TooLarge 不算 Full

    auto r = p.TryReserve(max); // 正好等于上限：放得下
    CHECK(r.status == ReserveStatus::kOk);
    FillPayload(r.payload, 5);
    p.Commit();
    ExpectOne(c, max, 5);

    // 握手时可以把上限调小。
    ChannelOptions o;
    o.tx_capacity = kMinRingCapacity;
    o.tx_max_payload = 100;
    auto small = ShmChannel::Create(o);
    CHECK(small.has_value());
    RingProducer sp = small->TxProducer();
    CHECK(sp.TryReserve(100).status == ReserveStatus::kOk);
    CHECK(sp.TryReserve(101).status == ReserveStatus::kTooLarge);
}

// 记录跨过环尾：随机长度写很多圈，逐条核对。镜像映射保证跨尾的记录在虚拟地址上连续，调用方不用拼接。
void TestWrapAroundMirror()
{
    ShmChannel ch = MakeChannel();
    RingProducer p = ch.TxProducer();
    RingConsumer c = ch.TxConsumer();
    std::mt19937_64 rng(12345);
    std::uint64_t crossings = 0;
    std::uint64_t seq = 0;
    std::uint64_t written = 0;
    const std::uint64_t mask = kMinRingCapacity - 1;

    constexpr std::uint64_t kLaps = 100;
    while (written < kLaps * kMinRingCapacity)
    {
        const std::uint32_t len = 8 + static_cast<std::uint32_t>(rng() % 3000);
        const std::uint64_t pos = ch.tx_control().write_idx.load() & mask;
        if (pos + RecordSize(len) > kMinRingCapacity)
            ++crossings;
        ProduceOne(p, len, seq);
        RecordView v;
        CHECK(c.Peek(v) == PeekStatus::kOk);
        std::uint64_t got = 0;
        CHECK(v.len == len && CheckPayload(v.payload, v.len, &got) && got == seq);
        c.Pop();
        ++seq;
        written += RecordSize(len);
    }
    // 环每绕一圈有一条记录跨过尾部（边界恰好落在两条记录之间的圈除外，约 4%）：确实覆盖到了，不是碰巧没跨过。
    std::printf("  wrap: crossings=%llu laps=%llu\n", (unsigned long long)crossings, (unsigned long long)kLaps);
    CHECK(crossings >= kLaps * 8 / 10);
}

// 游标是 64 位单调计数，不回绕；但测试要证明就算走到 2^64 也不出错：把起点设在 2^64 前面一点。
// 容量整除 2^64，所以位置 = idx & mask 在 2^64 → 0 处依然连续。
void TestCursorsNear2Pow64()
{
    ShmChannel ch = MakeChannel();
    RingControl& ctl = ch.tx_control();
    const std::uint64_t start = ~0ULL - 64 * 300 + 1; // 2^64 - 19200，64 的倍数
    CHECK(start % 64 == 0);
    ctl.write_idx.store(start);
    ctl.read_idx.store(start);

    RingProducer p = ch.TxProducer();
    RingConsumer c = ch.TxConsumer();
    CHECK(!p.corrupt() && !c.corrupt());

    bool crossed_zero = false;
    for (std::uint64_t i = 0; i < 2000; ++i)
    {
        ProduceOne(p, 48 + (i % 5) * 64, i);
        if (ctl.write_idx.load() < start)
            crossed_zero = true;
        ExpectOne(c, 48 + (i % 5) * 64, i);
    }
    CHECK(crossed_zero);
    CHECK(ctl.write_idx.load() == ctl.read_idx.load());

    // 在 2^64 附近填满再腾空：占用用无符号差算，不受回绕影响。
    ctl.write_idx.store(~0ULL - 63); // 2^64 - 64
    ctl.read_idx.store(~0ULL - 63);
    RingProducer p2 = ch.TxProducer();
    RingConsumer c2 = ch.TxConsumer();
    std::uint64_t n = 0;
    while (p2.TryReserve(48).status == ReserveStatus::kOk)
        ++n;
    p2.Commit();
    CHECK(n == kMinRingCapacity / 64);
    RecordView v;
    std::uint64_t m = 0;
    while (c2.Peek(v) == PeekStatus::kOk)
    {
        c2.Pop();
        ++m;
    }
    CHECK(m == n);

    // 游标靠近 2^64 时接手：哨兵 ~0 与游标的有符号差在这里会变成"超前"，必须靠显式认哨兵，否则被误报 Corrupt。
    const std::uint64_t base = ~0ULL - 127; // 2^64 - 128：两条记录之后回到 0
    ctl.write_idx.store(base);
    ctl.read_idx.store(base);
    ctl.inflight_idx.store(kNoInflight);
    {
        RingProducer p3 = ch.TxProducer();
        for (std::uint64_t i = 0; i < 4; ++i) // 前两条在 2^64 之前，后两条已经绕回 0
        {
            auto r = p3.TryReserve(48);
            CHECK(r.status == ReserveStatus::kOk);
            FillPayload(r.payload, i);
        }
        p3.Commit();
    }
    {
        RingConsumer fresh = ch.TxConsumer();
        CHECK(fresh.RecoverFromCrash() == RecoverStatus::kClean); // 哨兵 + 靠近 2^64 的游标：不是 Corrupt
        RecordView v;
        CHECK(fresh.Peek(v) == PeekStatus::kOk && v.cursor == base);
    }
    // 死在处理第二条（游标 2^64 - 64）的期间：接手方跳过它，下一条在绕回之后的游标 0 处。
    {
        RingConsumer old = ch.TxConsumer();
        RecordView v;
        CHECK(old.Peek(v) == PeekStatus::kOk);
        old.Pop();
        CHECK(old.Peek(v) == PeekStatus::kOk && v.cursor == ~0ULL - 63);
        old.MarkInflight(v);
    }
    RingConsumer next = ch.TxConsumer();
    CHECK(next.RecoverFromCrash() == RecoverStatus::kSkipped);
    RecordView v2;
    CHECK(next.Peek(v2) == PeekStatus::kOk && v2.cursor == 0);
    std::uint64_t seq2 = 0;
    CHECK(CheckPayload(v2.payload, v2.len, &seq2) && seq2 == 2);
}

void TestWakeProtocol()
{
    ShmChannel ch = MakeChannel();
    RingProducer p = ch.TxProducer();
    RingConsumer c = ch.TxConsumer();
    RingControl& ctl = ch.tx_control();
    const int efd = ch.tx_eventfd();
    CHECK(c.eventfd() == efd);

    // 消费者醒着（没有睡）：Commit 不敲 eventfd。
    ProduceOne(p, 40, 0);
    CHECK(DrainEventFd(efd) == 0);
    CHECK(ctl.producer_stats.wakeups_sent.Get() == 0);
    ExpectOne(c, 40, 0);

    // 环空，准备睡：可以睡，标志为 1。
    CHECK(c.PrepareToSleep());
    CHECK(ctl.consumer_sleeping.load() == 1);
    // 生产者 Commit：看到标志，敲一次 eventfd，并把标志清掉。
    ProduceOne(p, 40, 1);
    CHECK(ctl.consumer_sleeping.load() == 0);
    CHECK(ctl.producer_stats.wakeups_sent.Get() == 1);
    // 再来两次 Commit 不会重复敲（标志已被 exchange 成 0）。
    ProduceOne(p, 40, 2);
    ProduceOne(p, 40, 3);
    CHECK(ctl.producer_stats.wakeups_sent.Get() == 1);
    // 消费者醒来：读清 eventfd（计数是 1），清标志。
    c.WokeUp();
    CHECK(ctl.consumer_stats.wakeups_received.Get() == 1);
    CHECK(ctl.consumer_stats.spurious_wakeups.Get() == 0);
    CHECK(DrainEventFd(efd) == 0);
    for (std::uint64_t i = 1; i <= 3; ++i)
        ExpectOne(c, 40, i);
    CHECK(ctl.consumer_stats.empty_wakeups.Get() == 0); // 醒来之后有活干：不是空唤醒

    // 睡前回头一看，已经有数据：不能睡，标志被清回去，也不用敲 eventfd。
    ProduceOne(p, 40, 4);
    CHECK(!c.PrepareToSleep());
    CHECK(ctl.consumer_sleeping.load() == 0);
    CHECK(ctl.producer_stats.wakeups_sent.Get() == 1);
    ExpectOne(c, 40, 4);

    // 虚假唤醒：eventfd 空时 WokeUp 记 spurious，不阻塞。
    CHECK(c.PrepareToSleep());
    c.WokeUp();
    CHECK(ctl.consumer_stats.spurious_wakeups.Get() == 1);
    CHECK(ctl.consumer_sleeping.load() == 0);
    {
        RecordView v;
        CHECK(c.Peek(v) == PeekStatus::kEmpty);
        CHECK(ctl.consumer_stats.empty_wakeups.Get() == 0); // eventfd 本来就是空的：记 spurious，不记 empty
    }

    // 批量 Commit 只敲一次。
    CHECK(c.PrepareToSleep());
    for (std::uint64_t i = 0; i < 5; ++i)
    {
        auto r = p.TryReserve(40);
        CHECK(r.status == ReserveStatus::kOk);
        FillPayload(r.payload, 10 + i);
    }
    p.Commit();
    CHECK(ctl.producer_stats.wakeups_sent.Get() == 2);
    CHECK(DrainEventFd(efd) == 1);
    for (std::uint64_t i = 0; i < 5; ++i)
        ExpectOne(c, 40, 10 + i);

    // 旧计数：睡前把标志置 1，生产者敲了 eventfd，但消费者没经过 WokeUp 就把记录收走了（睡前回头一看那条路）。
    // 之后 epoll 因旧计数立刻返回：WokeUp 读到计数，第一次 Peek 是空的，记一次空唤醒；再 Peek 不重复计；随后可以真的睡，eventfd 已读清。
    CHECK(c.PrepareToSleep());
    ProduceOne(p, 40, 20);
    CHECK(ctl.producer_stats.wakeups_sent.Get() == 3);
    ExpectOne(c, 40, 20);
    CHECK(ctl.consumer_stats.empty_wakeups.Get() == 0);
    c.WokeUp();
    CHECK(ctl.consumer_stats.wakeups_received.Get() == 2);
    {
        RecordView v;
        CHECK(c.Peek(v) == PeekStatus::kEmpty);
        CHECK(ctl.consumer_stats.empty_wakeups.Get() == 1);
        CHECK(c.Peek(v) == PeekStatus::kEmpty);
        CHECK(ctl.consumer_stats.empty_wakeups.Get() == 1);
    }
    CHECK(ctl.consumer_stats.spurious_wakeups.Get() == 1);
    CHECK(c.PrepareToSleep());
    CHECK(DrainEventFd(efd) == 0);
}

// 双线程压力：生产者随机停顿，消费者按"搬空 → 自旋一小段 → PrepareToSleep → poll 等 eventfd"的协议工作。
// 每次 poll 的超时设得很长（10 秒）：只要有一次超时而生产者还没结束，或者结束了环里还有数据，就是丢了唤醒。
// slow_consumer：消费者每读一阵就停几毫秒，让小环被填满，走生产者遇到 Full 后 Commit + yield 重试的路径。
void TestWakeStress(std::uint64_t total, bool tiny_ring, bool slow_consumer)
{
    ChannelOptions o;
    o.tx_capacity = tiny_ring ? kMinRingCapacity : 4 * kMinRingCapacity;
    o.tx_max_payload = 512;
    auto chr = ShmChannel::Create(o);
    CHECK(chr.has_value());
    ShmChannel& ch = *chr;

    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> produced{0};

    std::thread producer(
        [&]
        {
            RingProducer p = ch.TxProducer();
            std::mt19937_64 rng(777);
            std::uint64_t seq = 0;
            while (seq < total)
            {
                // 一批 1~16 条；偶尔长停顿让消费者真的睡下去，再唤醒。
                const unsigned batch = 1 + static_cast<unsigned>(rng() % 16);
                for (unsigned i = 0; i < batch && seq < total; ++i)
                {
                    const std::uint32_t len = 8 + static_cast<std::uint32_t>(rng() % 200);
                    auto r = p.TryReserve(len);
                    while (r.status == ReserveStatus::kFull)
                    {
                        p.Commit();
                        std::this_thread::yield();
                        r = p.TryReserve(len);
                    }
                    CHECK(r.status == ReserveStatus::kOk);
                    FillPayload(r.payload, seq++);
                }
                p.Commit();
                produced.store(seq, std::memory_order_relaxed);
                const auto pause = rng() % 4096;
                if (pause == 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(3));
                else if (pause < 64)
                    std::this_thread::sleep_for(std::chrono::microseconds(pause));
            }
            done.store(true, std::memory_order_release);
        });

    RingConsumer c = ch.TxConsumer();
    std::uint64_t expect = 0;
    std::uint64_t sleeps = 0;
    while (expect < total)
    {
        RecordView v;
        const PeekStatus st = c.Peek(v);
        if (st == PeekStatus::kOk)
        {
            std::uint64_t seq = 0;
            CHECK(CheckPayload(v.payload, v.len, &seq));
            CHECK(seq == expect);
            ++expect;
            c.Pop();
            if (slow_consumer && expect % 3000 == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
            continue;
        }
        CHECK(st == PeekStatus::kEmpty);
        // 先自旋一小段，来了就继续。
        bool got = false;
        for (int i = 0; i < 200 && !got; ++i)
            got = c.Peek(v) == PeekStatus::kOk;
        if (got)
            continue;
        if (!c.PrepareToSleep())
            continue;
        ++sleeps;
        pollfd pfd{c.eventfd(), POLLIN, 0};
        const int n = ::poll(&pfd, 1, 10000);
        if (n == 0)
        {
            // 超时：生产者早该有新数据了。允许的唯一情况是它已经结束且我们读完了（循环条件会退出）。
            std::fprintf(stderr,
                         "丢了唤醒？expect=%llu produced=%llu done=%d\n",
                         (unsigned long long)expect,
                         (unsigned long long)produced.load(),
                         done.load() ? 1 : 0);
            std::abort();
        }
        CHECK(n == 1);
        c.WokeUp();
    }
    producer.join();
    CHECK(expect == total);
    RecordView v;
    CHECK(c.Peek(v) == PeekStatus::kEmpty);
    CHECK(ch.tx_control().producer_stats.records_committed.Get() == total);
    CHECK(ch.tx_control().consumer_stats.records_popped.Get() == total);
    std::printf("  wake stress: total=%llu sleeps=%llu wakeups_sent=%llu full_events=%llu\n",
                (unsigned long long)total,
                (unsigned long long)sleeps,
                (unsigned long long)ch.tx_control().producer_stats.wakeups_sent.Get(),
                (unsigned long long)ch.tx_control().producer_stats.full_events.Get());
    CHECK(sleeps > 0); // 确实睡过，不是一路自旋过来的
    if (slow_consumer)
        CHECK(ch.tx_control().producer_stats.full_events.Get() > 0); // 确实走过 Full 重试
}

// 专门逼 Dekker 的两道栅栏：消费者一读空就睡（不自旋），生产者以几十到几百纳秒的随机间隔逐条 Commit，
// 于是"写自己的标志、读对方的标志"在两边几乎同时发生。丢一次唤醒，消费者就在 poll 里睡到超时。
// x86 的 TSO 允许"先写后读"被重排，所以缺栅栏只能靠概率撞出来：这是尽力而为的检测，不是证明；
// 真正的保证要靠内存模型的推理加真机上的 TSAN（设计文档 §12）。
// 不要绑核：线程会继承创建者的亲和性，而这个测试进程是单进程多个用例顺序跑的——前一个用例把主线程绑死在某个核上，
// 后一个用例新建的线程就继承了，两个锁步自旋的线程落在同一个核上，每次屏障交接都要等调度器抢占，慢到几乎不动
// （实测 2 万轮 20 秒都跑不完，分核时 0.6 us/轮）。
void TestWakeHammer(std::uint64_t total)
{
    ShmChannel ch = MakeChannel();
    std::thread producer(
        [&]
        {
            RingProducer p = ch.TxProducer();
            std::mt19937_64 rng(99);
            for (std::uint64_t seq = 0; seq < total; ++seq)
            {
                auto r = p.TryReserve(16);
                while (r.status == ReserveStatus::kFull)
                {
                    std::this_thread::yield();
                    r = p.TryReserve(16);
                }
                CHECK(r.status == ReserveStatus::kOk);
                FillPayload(r.payload, seq);
                p.Commit();
                const unsigned gap = static_cast<unsigned>(rng() % 300);
                for (volatile unsigned i = 0; i < gap; i = i + 1)
                {
                }
            }
        });

    RingConsumer c = ch.TxConsumer();
    std::uint64_t sleeps = 0;
    for (std::uint64_t expect = 0; expect < total;)
    {
        RecordView v;
        if (c.Peek(v) == PeekStatus::kOk)
        {
            std::uint64_t seq = 0;
            CHECK(CheckPayload(v.payload, v.len, &seq) && seq == expect);
            c.Pop();
            ++expect;
            continue;
        }
        if (!c.PrepareToSleep()) // 不自旋：一读空就走睡眠协议
            continue;
        ++sleeps;
        pollfd pfd{c.eventfd(), POLLIN, 0};
        if (::poll(&pfd, 1, 5000) == 0)
        {
            std::fprintf(stderr,
                         "hammer: 丢了唤醒？expect=%llu sleeps=%llu\n",
                         (unsigned long long)expect,
                         (unsigned long long)sleeps);
            std::abort();
        }
        c.WokeUp();
    }
    producer.join();
    std::printf("  wake hammer: total=%llu sleeps=%llu wakeups_sent=%llu\n",
                (unsigned long long)total,
                (unsigned long long)sleeps,
                (unsigned long long)ch.tx_control().producer_stats.wakeups_sent.Get());
    CHECK(sleeps >= total / 200); // 确实频繁地睡了，锤子有砸到点上
}

// litmus 式测试：把 Dekker 的两侧放到"同一时刻"去跑，直接判定唯一不允许的结果。
// 每一轮：生产者预留好一条记录；两个线程过一道自旋屏障，然后分别（带随机的几十纳秒抖动）执行
//   生产者 Commit      = 写 write_idx、栅栏、读 sleeping（需要时敲 eventfd）
//   消费者 PrepareToSleep = 写 sleeping、栅栏、读 write_idx
// 两边都做完之后判定：消费者"决定睡了"（返回 true）而生产者"没敲 eventfd"，就是丢了唤醒——消费者会睡死。
// 这比"压力测试等超时"准得多：缺任何一道栅栏时，x86 的 store buffer 会让这个结果在几万轮内出现。
// 仍然是统计性的检测，不是证明；内存序的保证靠推理加真机上的 TSAN（设计文档 §12）。
void TestWakeLitmus(std::uint64_t rounds)
{
    if (std::thread::hardware_concurrency() < 2)
        return; // 单核上两个线程不可能真的并行，这个测试没有意义
    ShmChannel ch = MakeChannel();
    RingControl& ctl = ch.tx_control();
    std::atomic<std::uint64_t> barrier{0};
    auto wait_barrier = [&](std::uint64_t n) // 两个线程每道屏障各加一，到 2n 即都到齐
    {
        barrier.fetch_add(1, std::memory_order_acq_rel);
        while (barrier.load(std::memory_order_acquire) < 2 * n)
        {
        }
    };
    auto jitter = [](std::mt19937_64& rng)
    {
        const unsigned n = static_cast<unsigned>(rng() % 48);
        for (volatile unsigned i = 0; i < n; i = i + 1)
        {
        }
    };

    std::thread producer(
        [&]
        {
            RingProducer p = ch.TxProducer();
            std::mt19937_64 rng(1);
            for (std::uint64_t r = 0; r < rounds; ++r)
            {
                auto res = p.TryReserve(48);
                CHECK(res.status == ReserveStatus::kOk);
                FillPayload(res.payload, r);
                wait_barrier(3 * r + 1); // 起跑
                jitter(rng);
                p.Commit();
                wait_barrier(3 * r + 2); // 两边都做完
                wait_barrier(3 * r + 3); // 消费者收拾完
            }
        });

    RingConsumer c = ch.TxConsumer();
    std::mt19937_64 rng(2);
    std::uint64_t lost = 0, slept = 0, prev_sent = 0;
    for (std::uint64_t r = 0; r < rounds; ++r)
    {
        wait_barrier(3 * r + 1);
        jitter(rng);
        const bool will_sleep = c.PrepareToSleep();
        wait_barrier(3 * r + 2);
        const std::uint64_t sent = ctl.producer_stats.wakeups_sent.Get();
        if (will_sleep)
        {
            ++slept;
            if (sent == prev_sent)
                ++lost; // 决定睡了，却没人敲 eventfd
        }
        prev_sent = sent;
        c.WokeUp(); // 清标志、读清 eventfd（不论这一轮睡没睡，生产者都可能已经敲过）
        RecordView v;
        CHECK(c.Peek(v) == PeekStatus::kOk);
        c.Pop();
        wait_barrier(3 * r + 3);
    }
    producer.join();
    std::printf("  wake litmus: rounds=%llu 消费者决定睡的轮数=%llu 丢唤醒=%llu\n",
                (unsigned long long)rounds,
                (unsigned long long)slept,
                (unsigned long long)lost);
    CHECK(lost == 0);
    CHECK(slept > rounds / 100); // 两种结局都出现过：确实有大量轮次落在竞争窗口附近，不是一边倒
}

// 两条环互相独立：Tx 与 Rx 各有自己的标志、eventfd 与游标。
void TestTwoRingsIndependent()
{
    ShmChannel ch = MakeChannel(kMinRingCapacity, 2 * kMinRingCapacity);
    RingProducer tx_p = ch.TxProducer();
    RingConsumer tx_c = ch.TxConsumer();
    RingProducer rx_p = ch.RxProducer();
    RingConsumer rx_c = ch.RxConsumer();

    CHECK(tx_c.PrepareToSleep());
    CHECK(rx_c.PrepareToSleep());
    ProduceOne(rx_p, 40, 1); // 只唤醒 Rx 的消费者
    CHECK(ch.rx_control().consumer_sleeping.load() == 0);
    CHECK(ch.tx_control().consumer_sleeping.load() == 1);
    CHECK(DrainEventFd(ch.rx_eventfd()) == 1);
    CHECK(DrainEventFd(ch.tx_eventfd()) == 0);
    ExpectOne(rx_c, 40, 1);
    RecordView v;
    CHECK(tx_c.Peek(v) == PeekStatus::kEmpty);
    CHECK(ch.layout().rx.capacity == 2 * kMinRingCapacity);
    CHECK(ch.BumpAppEpoch() == 1 && ch.BumpAppEpoch() == 2 && ch.BumpAgentEpoch() == 1);
    (void)tx_p;
}

} // namespace

int main()
{
    Arm(200);
    TestEmptyAndRoundTrip();
    TestBatchPublishAndRelease();
    TestFullAndExactlyFull();
    TestTooLargeAndMaxPayload();
    TestWrapAroundMirror();
    TestCursorsNear2Pow64();
    TestWakeProtocol();
    TestTwoRingsIndependent();
#ifdef NDEBUG
    constexpr std::uint64_t kStress = 3'000'000;
    constexpr std::uint64_t kHammer = 3'000'000;
    constexpr std::uint64_t kLitmus = 3'000'000;
#else
    constexpr std::uint64_t kStress = 400'000;
    constexpr std::uint64_t kHammer = 500'000;
    constexpr std::uint64_t kLitmus = 400'000;
#endif
    TestWakeStress(kStress, /*tiny_ring=*/false, /*slow_consumer=*/false);
    TestWakeStress(kStress / 2, /*tiny_ring=*/true, /*slow_consumer=*/true);
    TestWakeHammer(kHammer);
    TestWakeLitmus(kLitmus);
    std::puts("shm_ring_test: OK");
    return 0;
}
