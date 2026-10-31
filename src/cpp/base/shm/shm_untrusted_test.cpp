// 对端不可信（设计文档 §7、§6.5）：直接篡改共享内存里的游标、记录头、通道头，以及交来的 fd，
// 核对每一项校验都会触发"拒绝 / Corrupt"，而不是崩溃、越界读写或悄悄放行。末尾有随机翻字节的模糊测试，
// 配合 asan / ubsan 预设跑才有意义（越界会被抓到）。
//
// 篡改记录头的办法：TryReserve 返回的载荷指针往前 16 字节就是记录头，Commit 之前或之后改都行。

#include <fcntl.h>
#include <sys/eventfd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "shm_channel.h"
#include "shm_memfd.h"
#include "shm_test_util.h"

using namespace breeze::shm;
using namespace breeze::shm::testing;

namespace
{

RecordHeader* HeaderOf(const Reserved& r)
{
    return reinterpret_cast<RecordHeader*>(r.payload.data() - kRecordHeaderSize);
}

// ---- 消费者看到的篡改 ----

// 放一条合法记录并发布，返回它的头（可继续篡改）。
RecordHeader* PutRecord(RingProducer& p, std::uint32_t len, std::uint64_t seq)
{
    auto r = p.TryReserve(len);
    CHECK(r.status == ReserveStatus::kOk);
    FillPayload(r.payload, seq);
    RecordHeader* h = HeaderOf(r);
    p.Commit();
    return h;
}

void ExpectConsumerCorrupt(RingConsumer& c)
{
    RecordView v;
    CHECK(c.Peek(v) == PeekStatus::kCorrupt);
    CHECK(c.corrupt());
}

void TestConsumerHeaderTamper()
{
    struct Case
    {
        const char* name;
        void (*tamper)(RecordHeader*);
    };
    const Case cases[] = {
        {"reserved 非零", [](RecordHeader* h) { h->reserved = 1; }},
        {"flags 非零", [](RecordHeader* h) { h->flags = 0x8000; }},
        {"len 超过 max_payload", [](RecordHeader* h) { h->len = kMinRingCapacity / 16 + 1; }},
        {"len 巨大", [](RecordHeader* h) { h->len = 0xFFFFFFFFU; }},
        {"记录越过 write_idx", [](RecordHeader* h) { h->len = 5000; }}, // 合法范围内，但超出已发布的部分
    };
    for (const Case& cs : cases)
    {
        ShmChannel ch = MakeChannel();
        RingProducer p = ch.TxProducer();
        RingConsumer c = ch.TxConsumer();
        RecordHeader* h = PutRecord(p, 100, 1);
        cs.tamper(h);
        std::fprintf(stderr, "  consumer tamper: %s\n", cs.name);
        ExpectConsumerCorrupt(c);
        // 锁定：之后即使把头改回去，也不再恢复（不尝试修复）。
        h->len = 100;
        h->reserved = 0;
        h->flags = 0;
        RecordView v;
        CHECK(c.Peek(v) == PeekStatus::kCorrupt);
        c.Pop(); // Corrupt 之后 Pop 不会推进
        CHECK(ch.tx_control().read_idx.load() == 0);
        // 恢复角色需要重新建对象，新对象从游标重新读，头已改回去所以能读
        RingConsumer fresh = ch.TxConsumer();
        CHECK(fresh.Peek(v) == PeekStatus::kOk);
    }
}

// len 超过 max_payload 要被单独拒绝：积压得够多（可用字节超过这条记录声称的大小），
// "记录越过 write_idx"那条检查就帮不上忙，只剩载荷上限在起作用。
void TestConsumerLenLimitWithBacklog()
{
    ShmChannel ch = MakeChannel();
    RingProducer p = ch.TxProducer();
    RingConsumer c = ch.TxConsumer();
    const std::uint32_t over = ch.layout().tx.max_payload + 1;
    RecordHeader* first = PutRecord(p, 100, 0);
    for (std::uint64_t i = 1; i < 1200; ++i)
        PutRecord(p, 100, i);
    CHECK(ch.tx_control().write_idx.load() > RecordSize(over)); // 积压确实比这条"声称的大小"还大
    first->len = over;
    ExpectConsumerCorrupt(c);
}

void TestConsumerCursorTamper()
{
    { // write_idx 比 read_idx 超前整整一圈以上：占用超过容量。
        ShmChannel ch = MakeChannel();
        RingProducer p = ch.TxProducer();
        RingConsumer c = ch.TxConsumer();
        PutRecord(p, 100, 1);
        ch.tx_control().write_idx.store(kMinRingCapacity + 64);
        ExpectConsumerCorrupt(c);
    }
    { // write_idx 倒退到已见过的值之前。
        ShmChannel ch = MakeChannel();
        RingProducer p = ch.TxProducer();
        RingConsumer c = ch.TxConsumer();
        PutRecord(p, 100, 1);
        PutRecord(p, 100, 2);
        RecordView v;
        CHECK(c.Peek(v) == PeekStatus::kOk);
        c.Pop();
        ch.tx_control().write_idx.store(RecordSize(100)); // 比见过的 2 × RecordSize(100) 小
        ExpectConsumerCorrupt(c);
    }
    { // write_idx 落在消费者游标后面（倒退到已读之前）。
        ShmChannel ch = MakeChannel();
        RingProducer p = ch.TxProducer();
        RingConsumer c = ch.TxConsumer();
        PutRecord(p, 100, 1);
        PutRecord(p, 100, 2);
        RecordView v;
        CHECK(c.Peek(v) == PeekStatus::kOk);
        c.Pop();
        CHECK(c.Peek(v) == PeekStatus::kOk);
        c.Pop();
        ch.tx_control().write_idx.store(0);
        ExpectConsumerCorrupt(c);
    }
    { // 不到一个记录头那么长的"数据"。
        ShmChannel ch = MakeChannel();
        RingConsumer c = ch.TxConsumer();
        ch.tx_control().write_idx.store(10);
        ExpectConsumerCorrupt(c);
    }
    { // 构造时就不合法：write_idx 小于 read_idx、占用超过容量、没对齐。
        ShmChannel a = MakeChannel();
        a.tx_control().read_idx.store(640);
        a.tx_control().write_idx.store(0);
        CHECK(a.TxConsumer().corrupt() && a.TxProducer().corrupt());
        ShmChannel b = MakeChannel();
        b.tx_control().write_idx.store(2 * kMinRingCapacity);
        CHECK(b.TxConsumer().corrupt() && b.TxProducer().corrupt());
        ShmChannel d = MakeChannel();
        d.tx_control().write_idx.store(100);
        CHECK(d.TxConsumer().corrupt() && d.TxProducer().corrupt());
        RecordView v;
        RingConsumer bad = d.TxConsumer();
        CHECK(bad.Peek(v) == PeekStatus::kCorrupt);
        RingProducer badp = d.TxProducer();
        CHECK(badp.TryReserve(10).status == ReserveStatus::kCorrupt);
    }
}

// ---- 生产者看到的篡改：消费者写的 read_idx ----

void FillRing(RingProducer& p)
{
    while (p.TryReserve(48).status == ReserveStatus::kOk)
    {
    }
    p.Commit();
}

void TestProducerCursorTamper()
{
    { // read_idx 超过 write_idx：消费者不可能消费还没发布的记录。
        ShmChannel ch = MakeChannel();
        RingProducer p = ch.TxProducer();
        FillRing(p);
        const std::uint64_t w = ch.tx_control().write_idx.load();
        ch.tx_control().read_idx.store(w + 64);
        CHECK(p.TryReserve(48).status == ReserveStatus::kCorrupt);
        CHECK(p.corrupt());
        ch.tx_control().read_idx.store(w); // 改回去也不再恢复
        CHECK(p.TryReserve(48).status == ReserveStatus::kCorrupt);
    }
    { // read_idx 倒退。
        ShmChannel ch = MakeChannel();
        RingProducer p = ch.TxProducer();
        RingConsumer c = ch.TxConsumer();
        FillRing(p);
        RecordView v;
        for (int i = 0; i < 10; ++i)
        {
            CHECK(c.Peek(v) == PeekStatus::kOk);
            c.Pop();
        }
        CHECK(p.TryReserve(48).status == ReserveStatus::kOk); // 重读 read_idx，缓存更新到 10 条之后
        ch.tx_control().read_idx.store(64);
        // 先把预留的这些用掉再触发重读：连续预留直到缓存的空间用完。
        ReserveStatus st = ReserveStatus::kOk;
        for (int i = 0; i < 100 && st == ReserveStatus::kOk; ++i)
            st = p.TryReserve(48).status;
        CHECK(st == ReserveStatus::kCorrupt);
    }
    { // read_idx 没对齐。
        ShmChannel ch = MakeChannel();
        RingProducer p = ch.TxProducer();
        FillRing(p);
        ch.tx_control().read_idx.store(33);
        CHECK(p.TryReserve(48).status == ReserveStatus::kCorrupt);
    }
}

// ---- 崩溃后接手：inflight 标记 ----

void TestRecoverFromCrash()
{
    { // 初值是哨兵，不是 0：第一条记录还在时接手，不能被误跳过。
        ShmChannel ch = MakeChannel();
        CHECK(ch.tx_control().inflight_idx.load() == kNoInflight);
        RingProducer p = ch.TxProducer();
        PutRecord(p, 40, 0);
        RingConsumer c = ch.TxConsumer();
        CHECK(c.RecoverFromCrash() == RecoverStatus::kClean);
        RecordView v;
        CHECK(c.Peek(v) == PeekStatus::kOk && v.cursor == 0);
    }
    { // 死在处理第一条的期间：新消费者跳过它，下一条照常。
        ShmChannel ch = MakeChannel();
        RingProducer p = ch.TxProducer();
        for (std::uint64_t i = 0; i < 3; ++i)
            PutRecord(p, 40, i);
        {
            RingConsumer old = ch.TxConsumer();
            RecordView v;
            CHECK(old.Peek(v) == PeekStatus::kOk);
            old.MarkInflight(v);
            // 没有 Pop：模拟进程在处理这条的时候被杀。
        }
        RingConsumer c = ch.TxConsumer();
        CHECK(c.RecoverFromCrash() == RecoverStatus::kSkipped);
        CHECK(ch.tx_control().consumer_stats.inflight_skipped.Get() == 1);
        CHECK(ch.tx_control().inflight_idx.load() == kNoInflight);
        RecordView v;
        CHECK(c.Peek(v) == PeekStatus::kOk);
        std::uint64_t seq = 0;
        CHECK(CheckPayload(v.payload, v.len, &seq) && seq == 1); // 序号 0 被跳过
        c.Pop();
        CHECK(c.RecoverFromCrash() == RecoverStatus::kClean); // 已经清掉，再调不会再跳
        CHECK(c.Peek(v) == PeekStatus::kOk);
        CHECK(CheckPayload(v.payload, v.len, &seq) && seq == 2);
    }
    { // 死在两条记录之间（已 Pop，还没标记下一条）：什么都不跳。
        ShmChannel ch = MakeChannel();
        RingProducer p = ch.TxProducer();
        for (std::uint64_t i = 0; i < 3; ++i)
            PutRecord(p, 40, i);
        {
            RingConsumer old = ch.TxConsumer();
            RecordView v;
            CHECK(old.Peek(v) == PeekStatus::kOk);
            old.MarkInflight(v);
            old.Pop(); // 标记落后于 read_idx
        }
        RingConsumer c = ch.TxConsumer();
        CHECK(c.RecoverFromCrash() == RecoverStatus::kClean);
        RecordView v;
        CHECK(c.Peek(v) == PeekStatus::kOk);
        std::uint64_t seq = 0;
        CHECK(CheckPayload(v.payload, v.len, &seq) && seq == 1);
    }
    { // 背压：标记之后决定先不处理，ClearInflight；此时崩溃不能跳过这条没处理过的。
        ShmChannel ch = MakeChannel();
        RingProducer p = ch.TxProducer();
        PutRecord(p, 40, 0);
        {
            RingConsumer old = ch.TxConsumer();
            RecordView v;
            CHECK(old.Peek(v) == PeekStatus::kOk);
            old.MarkInflight(v);
            old.ClearInflight();
        }
        RingConsumer c = ch.TxConsumer();
        CHECK(c.RecoverFromCrash() == RecoverStatus::kClean);
        RecordView v;
        CHECK(c.Peek(v) == PeekStatus::kOk);
    }
    { // 标记了但环里没有记录（对端乱写）：没有东西可跳，清掉标记。
        ShmChannel ch = MakeChannel();
        ch.tx_control().inflight_idx.store(0);
        RingConsumer c = ch.TxConsumer();
        CHECK(c.RecoverFromCrash() == RecoverStatus::kClean);
        CHECK(ch.tx_control().inflight_idx.load() == kNoInflight);
    }
    { // 标记超前于游标：不可能出现，按页被写坏处理。
        ShmChannel ch = MakeChannel();
        RingProducer p = ch.TxProducer();
        PutRecord(p, 40, 0);
        ch.tx_control().inflight_idx.store(640);
        RingConsumer c = ch.TxConsumer();
        CHECK(c.RecoverFromCrash() == RecoverStatus::kCorrupt);
        CHECK(c.corrupt());
    }
    { // 要跳的那条记录头已被写坏：不跳，报 Corrupt。
        ShmChannel ch = MakeChannel();
        RingProducer p = ch.TxProducer();
        RecordHeader* h = PutRecord(p, 40, 0);
        ch.tx_control().inflight_idx.store(0);
        h->reserved = 9;
        RingConsumer c = ch.TxConsumer();
        CHECK(c.RecoverFromCrash() == RecoverStatus::kCorrupt);
        CHECK(ch.tx_control().read_idx.load() == 0); // 没有推进
    }
}

// ---- Attach：对端交来的通道 ----

struct AttachCase
{
    const char* name;
    void (*tamper)(ShmChannel&);
    int expect_err; // 0 表示应当成功
};

void ExpectAttach(const AttachCase& cs, const AttachLimits& limits = {})
{
    ShmChannel ch = MakeChannel(2 * kMinRingCapacity, kMinRingCapacity);
    cs.tamper(ch);
    auto r = AttachDup(ch, limits);
    if (cs.expect_err == 0)
    {
        if (!r.has_value())
            std::fprintf(stderr, "  attach case [%s]: 期望成功，实际错误 %d\n", cs.name, r.error());
        CHECK(r.has_value());
    }
    else
    {
        if (r.has_value() || r.error() != cs.expect_err)
            std::fprintf(stderr,
                         "  attach case [%s]: 期望 %d，实际 %d\n",
                         cs.name,
                         cs.expect_err,
                         r.has_value() ? 0 : r.error());
        CHECK(!r.has_value() && r.error() == cs.expect_err);
    }
}

void TestAttachHeaderTamper()
{
    const AttachCase cases[] = {
        {"未篡改", [](ShmChannel&) {}, 0},
        {"魔数", [](ShmChannel& c) { c.header().fixed.magic ^= 1; }, EPROTO},
        {"layout_version", [](ShmChannel& c) { c.header().fixed.layout_version = 2; }, EPROTO},
        {"header_size", [](ShmChannel& c) { c.header().fixed.header_size += 64; }, EPROTO},
        {"page_size", [](ShmChannel& c) { c.header().fixed.page_size *= 2; }, EPROTO},
        {"record_align", [](ShmChannel& c) { c.header().fixed.record_align = 16; }, EPROTO},
        {"flags 非零", [](ShmChannel& c) { c.header().fixed.flags = 1; }, EINVAL},
        {"reserved 非零", [](ShmChannel& c) { c.header().fixed.reserved = 1; }, EINVAL},
        {"tx.reserved 非零", [](ShmChannel& c) { c.header().fixed.tx.reserved = 1; }, EINVAL},
        {"rx.reserved 非零", [](ShmChannel& c) { c.header().fixed.rx.reserved = 1; }, EINVAL},
        {"tx 容量不是 2 的幂", [](ShmChannel& c) { c.header().fixed.tx.capacity = 3 * kMinRingCapacity; }, EINVAL},
        {"tx 容量低于下限", [](ShmChannel& c) { c.header().fixed.tx.capacity = kMinRingCapacity / 2; }, EINVAL},
        {"tx 容量为 0", [](ShmChannel& c) { c.header().fixed.tx.capacity = 0; }, EINVAL},
        {"tx 容量翻倍（与文件大小对不上）", [](ShmChannel& c) { c.header().fixed.tx.capacity *= 2; }, EINVAL},
        {"rx 容量超过配额上限",
         [](ShmChannel& c) { c.header().fixed.rx.capacity = 2 * kDefaultMaxRingCapacity; },
         EINVAL},
        {"tx.max_payload 为 0", [](ShmChannel& c) { c.header().fixed.tx.max_payload = 0; }, EINVAL},
        {"tx.max_payload 超过容量 1/16",
         [](ShmChannel& c)
         { c.header().fixed.tx.max_payload = static_cast<std::uint32_t>(2 * kMinRingCapacity / 16 + 1); },
         EINVAL},
        {"total_size", [](ShmChannel& c) { c.header().fixed.total_size += 4096; }, EINVAL},
        {"tx.control_offset", [](ShmChannel& c) { c.header().fixed.tx.control_offset += 4096; }, EINVAL},
        {"tx.data_offset", [](ShmChannel& c) { c.header().fixed.tx.data_offset += 4096; }, EINVAL},
        {"rx.control_offset", [](ShmChannel& c) { c.header().fixed.rx.control_offset = 0; }, EINVAL},
        {"rx.data_offset", [](ShmChannel& c) { c.header().fixed.rx.data_offset -= 4096; }, EINVAL},
        {"tx 占用超过容量", [](ShmChannel& c) { c.tx_control().write_idx.store(2 * kMinRingCapacity + 64); }, EINVAL},
        {"tx 游标没对齐", [](ShmChannel& c) { c.tx_control().write_idx.store(100); }, EINVAL},
        {"rx read_idx 超过 write_idx", [](ShmChannel& c) { c.rx_control().read_idx.store(64); }, EINVAL},
        {"inflight 超过 write_idx", [](ShmChannel& c) { c.tx_control().inflight_idx.store(640); }, EINVAL},
        {"inflight 没对齐（write_idx 之内）",
         [](ShmChannel& c)
         {
             c.rx_control().write_idx.store(640);
             c.rx_control().inflight_idx.store(100);
         },
         EINVAL},
        {"inflight 在 read_idx 与 write_idx 之间：接手方接受（精确关系留给 Recover 查）",
         [](ShmChannel& c)
         {
             c.rx_control().write_idx.store(640);
             c.rx_control().inflight_idx.store(320);
         },
         0},
        {"consumer_sleeping 越界", [](ShmChannel& c) { c.tx_control().consumer_sleeping.store(7); }, EINVAL},
        {"带着合法的积压（游标不在 0）接手",
         [](ShmChannel& c)
         {
             RingProducer p = c.TxProducer();
             for (std::uint64_t i = 0; i < 5; ++i)
                 PutRecord(p, 100, i);
         },
         0},
    };
    for (const AttachCase& cs : cases)
        ExpectAttach(cs);

    // 配额：接手方的单环上限比通道里的容量小，拒绝；刚好等于，接受。
    AttachLimits tight;
    tight.max_ring_capacity = kMinRingCapacity; // 通道里 tx 是 2 MiB
    ExpectAttach({"容量超过本端配额", [](ShmChannel&) {}, EINVAL}, tight);
    AttachLimits exact;
    exact.max_ring_capacity = 2 * kMinRingCapacity;
    ExpectAttach({"容量等于本端配额", [](ShmChannel&) {}, 0}, exact);
}

// 接手方接受 inflight 落在 read_idx 与 write_idx 之间，但成为消费者之后 RecoverFromCrash 要把它报成 Corrupt：
// 旧消费者已经不在，标记超前于游标不可能由正常流程产生。
void TestInflightBetweenCursorsCorruptOnRecover()
{
    ShmChannel ch = MakeChannel();
    RingProducer p = ch.TxProducer();
    for (std::uint64_t i = 0; i < 5; ++i)
        PutRecord(p, 40, i);
    ch.tx_control().inflight_idx.store(RecordSize(40) * 2); // 游标在 0，标记在第三条
    auto other = AttachDup(ch);
    CHECK(other.has_value());
    RingConsumer c = other->TxConsumer();
    CHECK(c.RecoverFromCrash() == RecoverStatus::kCorrupt);
}

// 文件比头里声明的总大小小：映射得上，但一访问被截掉的页就是 SIGBUS。
// 封印齐全、头也自洽，只有"文件大小等于总大小"这一条能拦住。
void TestAttachFileSmallerThanHeader()
{
    ShmChannel ch = MakeChannel();
    const std::uint64_t page = ch.layout().page_size;
    auto small = CreateMemFd("untrusted-small", ch.layout().total_size / 2);
    CHECK(small.has_value());
    // 把合法的头和控制块（3 页）原样拷进去，头里声明的总大小仍是原来的。
    CHECK(::pwrite(small->get(), &ch.header(), 3 * page, 0) == static_cast<ssize_t>(3 * page));
    CHECK(SealMemFd(small->get()) == 0);
    auto r = ShmChannel::Attach(std::move(*small), Fd(::dup(ch.tx_eventfd())), Fd(::dup(ch.rx_eventfd())));
    CHECK(!r.has_value() && r.error() == EINVAL);
}

// 回归：对端还活着、在全速推进游标和 inflight 标记时反复接手，不能被误拒。
// 校验里"较晚读到的一侧在合法状态下只会更大"的顺序一旦弄反（比如把 inflight 放到游标之后再读），这里会间歇性失败。
void TestAttachWhilePeersRunning()
{
    ShmChannel ch = MakeChannel();
    std::atomic<bool> stop{false};
    std::thread producer(
        [&]
        {
            RingProducer p = ch.TxProducer();
            std::uint64_t seq = 0;
            while (!stop.load(std::memory_order_relaxed))
            {
                auto r = p.TryReserve(40);
                if (r.status == ReserveStatus::kOk)
                {
                    FillPayload(r.payload, seq++);
                    p.Commit();
                }
                else
                    std::this_thread::yield();
            }
        });
    std::thread consumer(
        [&]
        {
            RingConsumer c = ch.TxConsumer();
            while (!stop.load(std::memory_order_relaxed))
            {
                RecordView v;
                if (c.Peek(v) == PeekStatus::kOk)
                {
                    c.MarkInflight(v);
                    c.Pop();
                }
            }
        });
    int rejected = 0;
    for (int i = 0; i < 20000; ++i)
    {
        auto other = AttachDup(ch);
        if (!other.has_value())
            ++rejected;
    }
    stop.store(true);
    producer.join();
    consumer.join();
    if (rejected != 0)
        std::fprintf(stderr, "  对端运行时接手被误拒 %d 次\n", rejected);
    CHECK(rejected == 0);
}

void TestAttachSharesMemory()
{
    ShmChannel ch = MakeChannel();
    auto other = AttachDup(ch);
    CHECK(other.has_value());
    CHECK(other->layout().tx.capacity == ch.layout().tx.capacity);
    CHECK(other->layout().rx.data_offset == ch.layout().rx.data_offset);

    // 两个映射指向同一份内存：一边生产、另一边消费；epoch 也共享。
    RingProducer p = ch.TxProducer();
    RingConsumer c = other->TxConsumer();
    CHECK(!c.corrupt());
    PutRecord(p, 100, 42);
    RecordView v;
    CHECK(c.Peek(v) == PeekStatus::kOk);
    std::uint64_t seq = 0;
    CHECK(CheckPayload(v.payload, v.len, &seq) && seq == 42);
    c.Pop();
    CHECK(ch.BumpAppEpoch() == 1);
    CHECK(other->BumpAppEpoch() == 2);
    CHECK(ch.header().app_epoch.load() == 2);

    // eventfd 也是同一个：另一边消费者睡下，这一边的 Commit 能唤醒它。
    CHECK(c.PrepareToSleep());
    PutRecord(p, 40, 1);
    std::uint64_t cnt = 0;
    CHECK(::read(other->tx_eventfd(), &cnt, sizeof(cnt)) == static_cast<ssize_t>(sizeof(cnt)) && cnt == 1);
}

void TestAttachBadFds()
{
    ShmChannel ch = MakeChannel();
    auto dupfd = [](int fd) { return Fd(::dup(fd)); };

    // 无效 fd。
    CHECK(ShmChannel::Attach(Fd(), dupfd(ch.tx_eventfd()), dupfd(ch.rx_eventfd())).error() == EINVAL);
    CHECK(ShmChannel::Attach(dupfd(ch.memfd()), Fd(), dupfd(ch.rx_eventfd())).error() == EINVAL);
    CHECK(ShmChannel::Attach(dupfd(ch.memfd()), dupfd(ch.tx_eventfd()), Fd()).error() == EINVAL);

    // 不是封印的 memfd（没封印、普通文件、eventfd、管道）。
    auto unsealed = CreateMemFd("untrusted-unsealed", ch.layout().total_size);
    CHECK(unsealed.has_value());
    CHECK(ShmChannel::Attach(std::move(*unsealed), dupfd(ch.tx_eventfd()), dupfd(ch.rx_eventfd())).error() == EINVAL);
    CHECK(ShmChannel::Attach(Fd(::open("/dev/null", O_RDWR)), dupfd(ch.tx_eventfd()), dupfd(ch.rx_eventfd())).error() ==
          EINVAL);
    CHECK(ShmChannel::Attach(dupfd(ch.tx_eventfd()), dupfd(ch.tx_eventfd()), dupfd(ch.rx_eventfd())).error() == EINVAL);

    // eventfd 的位置放了别的东西：阻塞的 eventfd、非 eventfd 的 fd、memfd。
    Fd blocking(::eventfd(0, 0));
    CHECK(ShmChannel::Attach(dupfd(ch.memfd()), std::move(blocking), dupfd(ch.rx_eventfd())).error() == EINVAL);
    int pp[2];
    CHECK(::pipe2(pp, O_NONBLOCK) == 0);
    Fd rd(pp[0]), wr(pp[1]);
    CHECK(ShmChannel::Attach(dupfd(ch.memfd()), dupfd(ch.tx_eventfd()), std::move(wr)).error() == EINVAL);
    CHECK(ShmChannel::Attach(dupfd(ch.memfd()), dupfd(ch.memfd()), dupfd(ch.rx_eventfd())).error() == EINVAL);

    // 封印齐全的 memfd，但大小不对 / 内容不是我们的：太小、过大、全零（魔数不符）。
    auto sealed = [](std::uint64_t size)
    {
        auto fd = CreateMemFd("untrusted-sealed", size);
        CHECK(fd.has_value());
        CHECK(SealMemFd(fd->get()) == 0);
        return std::move(*fd);
    };
    CHECK(ShmChannel::Attach(sealed(4096), dupfd(ch.tx_eventfd()), dupfd(ch.rx_eventfd())).error() == EINVAL);
    CHECK(ShmChannel::Attach(sealed(1ULL << 40), dupfd(ch.tx_eventfd()), dupfd(ch.rx_eventfd())).error() == EINVAL);
    CHECK(ShmChannel::Attach(sealed(3 * 4096 + 2 * kMinRingCapacity), dupfd(ch.tx_eventfd()), dupfd(ch.rx_eventfd()))
              .error() == EPROTO);

    // 失败的接手不会影响原来的通道。
    RingProducer p = ch.TxProducer();
    RingConsumer c = ch.TxConsumer();
    PutRecord(p, 40, 9);
    RecordView v;
    CHECK(c.Peek(v) == PeekStatus::kOk);
}

// 随机翻字节：头与控制块里任何位置被写坏，Attach 要么拒绝，要么成功之后各种操作也不能崩溃、越界或挂死。
// 配合 asan / ubsan 预设跑才抓得到越界。种子固定，失败可复现。
void TestFuzz()
{
    ShmChannel ch = MakeChannel();
    const std::uint64_t control_bytes = 3ULL * ch.layout().page_size;
    // 控制块里真正有内容的范围：头、两个 RingControl（其余是页内空白，翻了没意义）。
    struct Range
    {
        std::uint64_t off;
        std::uint64_t len;
    };
    const Range ranges[] = {
        {0, sizeof(ChannelHeader)},
        {ch.layout().tx.control_offset, sizeof(RingControl)},
        {ch.layout().rx.control_offset, sizeof(RingControl)},
    };
    {
        // 先放一点合法数据，让游标和记录有内容。
        RingProducer p = ch.TxProducer();
        RingProducer rp = ch.RxProducer();
        for (std::uint64_t i = 0; i < 20; ++i)
        {
            PutRecord(p, 40 + i, i);
            PutRecord(rp, 60 + i, i);
        }
    }
    std::vector<std::byte> saved(control_bytes);
    auto* control = reinterpret_cast<std::byte*>(&ch.header());
    std::memcpy(saved.data(), control, control_bytes);

    std::mt19937_64 rng(20261031);
    std::uint64_t attached = 0, rejected = 0;
    for (int iter = 0; iter < 30000; ++iter)
    {
        const int flips = 1 + static_cast<int>(rng() % 4);
        for (int f = 0; f < flips; ++f)
        {
            const Range& r = ranges[rng() % 3];
            control[r.off + rng() % r.len] ^= static_cast<std::byte>(1U << (rng() % 8));
        }
        auto other = AttachDup(ch);
        if (other.has_value())
        {
            ++attached;
            // 对每个角色做一阵有上限的操作；不要求成功，只要求不崩、不越界、不死循环。
            RingConsumer tc = other->TxConsumer();
            RingConsumer rc = other->RxConsumer();
            RingProducer tp = other->TxProducer();
            RingProducer rp = other->RxProducer();
            tc.RecoverFromCrash();
            rc.RecoverFromCrash();
            RecordView v;
            for (int i = 0; i < 50; ++i)
            {
                if (tc.Peek(v) == PeekStatus::kOk)
                {
                    std::uint64_t sink = 0;
                    for (std::uint32_t k = 0; k < v.len; k += 64)
                        sink += static_cast<std::uint8_t>(v.payload[k]); // 真的读载荷，让 asan 看到访问
                    (void)sink;
                    tc.MarkInflight(v);
                    tc.Pop();
                }
                if (rc.Peek(v) == PeekStatus::kOk)
                    rc.PopNoRelease();
                auto r1 = tp.TryReserve(static_cast<std::uint32_t>(rng() % 200));
                if (r1.status == ReserveStatus::kOk)
                    std::memset(r1.payload.data(), 0xA5, r1.payload.size());
                auto r2 = rp.TryReserve(static_cast<std::uint32_t>(rng() % 200));
                if (r2.status == ReserveStatus::kOk)
                    std::memset(r2.payload.data(), 0x5A, r2.payload.size());
                if (i % 7 == 0)
                {
                    tp.Commit();
                    rp.Commit();
                    rc.Release();
                }
            }
            tc.PrepareToSleep();
            tc.WokeUp();
        }
        else
        {
            ++rejected;
        }
        std::memcpy(control, saved.data(), control_bytes); // 还原，下一轮从干净状态翻
    }
    std::printf("  fuzz: attached=%llu rejected=%llu\n", (unsigned long long)attached, (unsigned long long)rejected);
    CHECK(rejected > 0 && attached > 0); // 两条路径都走到了
}

void TestSealBlocksTruncate()
{
    ShmChannel ch = MakeChannel();
    const off_t size = static_cast<off_t>(ch.layout().total_size);
    CHECK(::ftruncate(ch.memfd(), 0) != 0 && errno == EPERM);
    CHECK(::ftruncate(ch.memfd(), size / 2) != 0 && errno == EPERM);
    CHECK(::ftruncate(ch.memfd(), size * 2) != 0 && errno == EPERM);
    // Attach 方拿到的是同一个文件，同样被封印。
    auto other = AttachDup(ch);
    CHECK(other.has_value());
    CHECK(::ftruncate(other->memfd(), 0) != 0 && errno == EPERM);
}

} // namespace

int main()
{
    Arm(200);
    TestConsumerHeaderTamper();
    TestConsumerLenLimitWithBacklog();
    TestConsumerCursorTamper();
    TestProducerCursorTamper();
    TestRecoverFromCrash();
    TestAttachHeaderTamper();
    TestInflightBetweenCursorsCorruptOnRecover();
    TestAttachFileSmallerThanHeader();
    TestAttachWhilePeersRunning();
    TestAttachSharesMemory();
    TestAttachBadFds();
    TestSealBlocksTruncate();
    TestFuzz();
    std::puts("shm_untrusted_test: OK");
    return 0;
}
