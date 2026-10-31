// 多进程与崩溃注入（设计文档 §12）：fork 出生产者 / 消费者经 memfd 通信，在任意时刻 kill -9，核对
// 不会出现半条记录、游标不乱、至多一次与跳过逻辑成立、接手方能接着读写。
// 子进程里的任何 CHECK 失败都是 SIGABRT，父进程会把它报成失败；整个测试靠闹钟兜底，不会挂住 ctest。
//
// 本文件里的耗时与吞吐只是顺带打印，**不是性能结论**：沙箱里调度噪声很大，基准要另做（设计文档 §10）。

#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include "shm_channel.h"
#include "shm_memfd.h"
#include "shm_test_util.h"

using namespace breeze::shm;
using namespace breeze::shm::testing;

namespace
{

using Clock = std::chrono::steady_clock;

template <typename T>
T* SharedAlloc(std::size_t n)
{
    void* p = ::mmap(nullptr, n * sizeof(T), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED);
    return static_cast<T*>(p); // 匿名映射是全零
}

template <typename F>
pid_t Spawn(F f)
{
    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0)
    {
        Arm(150); // 子进程不继承闹钟
        f();
        ::_exit(0);
    }
    return pid;
}

int WaitStatus(pid_t pid)
{
    int status = 0;
    CHECK(::waitpid(pid, &status, 0) == pid);
    return status;
}

void ExpectExit0(pid_t pid, const char* what)
{
    const int st = WaitStatus(pid);
    if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0))
        std::fprintf(stderr, "子进程 [%s] 异常结束：status=0x%x\n", what, st);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

// 返回 true：被我们杀死了；返回 false：它在 kill 到达之前就自己正常退出了
// （退出码 0，比如消费者刚好读空、生产者刚好发完），
// 这是合法的竞态，调用方当作"它已经做完了"处理。其他任何结局（别的信号、非零退出码）都是子进程自己出了问题，直接失败。
bool KillAndReap(pid_t pid)
{
    ::kill(pid, SIGKILL);
    const int st = WaitStatus(pid);
    if (WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL)
        return true;
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
        return false;
    std::fprintf(stderr,
                 "子进程异常结束：status=0x%x exited=%d code=%d signaled=%d sig=%d\n",
                 st,
                 WIFEXITED(st),
                 WEXITSTATUS(st),
                 WIFSIGNALED(st),
                 WTERMSIG(st));
    CHECK(false);
    return false;
}

// 自旋 spin 次，再按睡眠协议 poll 等 eventfd。timeout_ms 内始终没有数据返回 false。
// 醒来或超时之后再 Peek 一次：超时了却有数据，说明丢了唤醒。
bool WaitPeek(RingConsumer& c, RecordView& v, int spin, int timeout_ms)
{
    for (;;)
    {
        if (c.Peek(v) == PeekStatus::kOk)
            return true;
        for (int i = 0; i < spin; ++i)
        {
            if (c.Peek(v) == PeekStatus::kOk)
                return true;
        }
        if (!c.PrepareToSleep())
            continue;
        pollfd pfd{c.eventfd(), POLLIN, 0};
        const int n = ::poll(&pfd, 1, timeout_ms);
        c.WokeUp();
        if (n == 0)
        {
            CHECK(c.Peek(v) != PeekStatus::kOk); // 超时了环里却有数据：丢了唤醒
            return false;
        }
    }
}

// ---- 1. 跨进程收发，接手方用 Attach 重新映射 ----

void TestCrossProcessStream()
{
    ShmChannel ch = MakeChannel();
    constexpr std::uint64_t kN = 300000;

    const pid_t producer = Spawn(
        [&]
        {
            // 子进程用 dup 出来的 fd 重新 Attach：新的映射地址，也走校验。
            auto mine = AttachDup(ch);
            CHECK(mine.has_value());
            RingProducer p = mine->TxProducer();
            std::mt19937_64 rng(5);
            for (std::uint64_t seq = 0; seq < kN; ++seq)
            {
                const std::uint32_t len = 8 + static_cast<std::uint32_t>(rng() % 500);
                auto r = p.TryReserve(len);
                while (r.status == ReserveStatus::kFull)
                {
                    p.Commit();
                    ::sched_yield();
                    r = p.TryReserve(len);
                }
                CHECK(r.status == ReserveStatus::kOk);
                FillPayload(r.payload, seq);
                if (seq % 5 == 4)
                    p.Commit();
                if (seq % 20000 == 19999)
                {
                    p.Commit();
                    ::usleep(2000); // 让消费者睡下去再被唤醒
                }
            }
            p.Commit();
        });

    RingConsumer c = ch.TxConsumer();
    for (std::uint64_t expect = 0; expect < kN; ++expect)
    {
        RecordView v;
        CHECK(WaitPeek(c, v, 100, 10000));
        std::uint64_t seq = 0;
        CHECK(CheckPayload(v.payload, v.len, &seq) && seq == expect);
        c.Pop();
    }
    ExpectExit0(producer, "producer");
    RecordView v;
    CHECK(c.Peek(v) == PeekStatus::kEmpty);
}

// ---- 2. 乒乓：两个方向、两个 eventfd，跨进程的唤醒 ----

void RunPingPong(std::uint64_t n, int spin, const char* label)
{
    ShmChannel ch = MakeChannel();
    const pid_t agent = Spawn(
        [&]
        {
            auto mine = AttachDup(ch);
            CHECK(mine.has_value());
            RingConsumer tc = mine->TxConsumer();
            RingProducer rp = mine->RxProducer();
            for (;;)
            {
                RecordView v;
                CHECK(WaitPeek(tc, v, spin, 10000));
                std::uint64_t seq = 0;
                CHECK(CheckPayload(v.payload, v.len, &seq));
                if (seq == ~0ULL)
                    return; // 结束标记
                tc.MarkInflight(v);
                auto r = rp.TryReserve(v.len);
                CHECK(r.status == ReserveStatus::kOk);
                std::memcpy(r.payload.data(), v.payload, v.len);
                rp.Commit();
                tc.Pop();
            }
        });

    RingProducer tp = ch.TxProducer();
    RingConsumer rc = ch.RxConsumer();
    const auto t0 = Clock::now();
    for (std::uint64_t i = 0; i < n; ++i)
    {
        auto r = tp.TryReserve(16);
        CHECK(r.status == ReserveStatus::kOk);
        FillPayload(r.payload, i);
        tp.Commit();
        RecordView v;
        CHECK(WaitPeek(rc, v, spin, 10000));
        std::uint64_t seq = 0;
        CHECK(CheckPayload(v.payload, v.len, &seq) && seq == i);
        rc.Pop();
    }
    const auto dt = Clock::now() - t0;
    auto r = tp.TryReserve(16);
    CHECK(r.status == ReserveStatus::kOk);
    FillPayload(r.payload, ~0ULL);
    tp.Commit();
    ExpectExit0(agent, "agent");

    const double us = std::chrono::duration<double, std::micro>(dt).count() / static_cast<double>(n);
    std::printf("  pingpong[%s]: n=%llu 平均往返 %.1f us（仅供参考）; Tx wakeups=%llu Rx wakeups=%llu\n",
                label,
                (unsigned long long)n,
                us,
                (unsigned long long)ch.tx_control().producer_stats.wakeups_sent.Get(),
                (unsigned long long)ch.rx_control().producer_stats.wakeups_sent.Get());
}

// ---- 3. 消费者被 kill -9：至多一次、跳过处理到一半的、不被毒包拖进崩溃循环 ----

std::uint32_t LenOf(std::uint64_t seq) { return 8 + static_cast<std::uint32_t>((seq * 2654435761ULL) % 120); }

struct ConsumerKillShared
{
    std::atomic<bool> producer_done;
    std::atomic<std::uint64_t> last_seq; // 上一任消费者处理到的最后一个序号；~0 表示还没有
};

void TestConsumerKilled()
{
#ifdef NDEBUG
    constexpr std::uint64_t kN = 400000;
#else
    constexpr std::uint64_t kN = 120000;
#endif
    ShmChannel ch = MakeChannel();
    auto* sh = SharedAlloc<ConsumerKillShared>(1);
    sh->last_seq.store(~0ULL);
    auto* processed = SharedAlloc<std::uint8_t>(kN);

    const pid_t producer = Spawn(
        [&]
        {
            RingProducer p = ch.TxProducer();
            for (std::uint64_t seq = 0; seq < kN; ++seq)
            {
                auto r = p.TryReserve(LenOf(seq));
                while (r.status == ReserveStatus::kFull)
                {
                    p.Commit();
                    ::sched_yield();
                    r = p.TryReserve(LenOf(seq));
                }
                CHECK(r.status == ReserveStatus::kOk);
                FillPayload(r.payload, seq);
                if (seq % 4 == 3)
                    p.Commit();
            }
            p.Commit();
            sh->producer_done.store(true, std::memory_order_release);
        });

    auto consumer_body = [&]
    {
        RingConsumer c = ch.TxConsumer();
        CHECK(!c.corrupt());
        CHECK(c.RecoverFromCrash() != RecoverStatus::kCorrupt);
        std::uint64_t n = 0;
        for (;;)
        {
            RecordView v;
            const PeekStatus st = c.Peek(v);
            CHECK(st != PeekStatus::kCorrupt);
            if (st == PeekStatus::kEmpty)
            {
                if (sh->producer_done.load(std::memory_order_acquire) && c.Peek(v) == PeekStatus::kEmpty)
                    return; // 生产者结束且读空：正常退出
                ::sched_yield();
                continue;
            }
            std::uint64_t seq = 0;
            CHECK(CheckPayload(v.payload, v.len, &seq)); // 不会有半条记录
            CHECK(v.len == LenOf(seq));
            const std::uint64_t last = sh->last_seq.load(std::memory_order_relaxed);
            CHECK(last == ~0ULL || seq > last); // 序号严格递增：不重发、不乱序（允许有空洞）

            c.MarkInflight(v);
            ++n;
            // —— 处理：下面记账。被杀在 Mark 与记账之间会丢这一条；被杀在记账与 Pop 之间，这一条会被接手方跳过。
            // 两个窗口本来都只有几十纳秒，各拉宽一次，让 kill 经常落在这里，两条路径都能被覆盖到。——
            if (n % 20 == 7)
                ::usleep(100);
            ++processed[seq];
            sh->last_seq.store(seq, std::memory_order_relaxed);
            if (n % 20 == 0)
                ::usleep(100);
            c.Pop();
        }
    };

    std::mt19937_64 rng(31337);
    std::uint64_t kills = 0;
    for (;;)
    {
        const pid_t cons = Spawn(consumer_body);
        ::usleep(static_cast<useconds_t>(500 + rng() % 12000));
        int st = 0;
        if (::waitpid(cons, &st, WNOHANG) == cons) // 自己退出了：必须是读空后的正常退出
        {
            CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
            break;
        }
        if (!KillAndReap(cons)) // kill 到达之前它刚好读空退出了：已经做完，不用再起下一任
            break;
        ++kills;
        CHECK(kills < 5000);
    }
    ExpectExit0(producer, "producer");

    std::uint64_t done = 0, dups = 0;
    for (std::uint64_t i = 0; i < kN; ++i)
    {
        if (processed[i] == 1)
            ++done;
        else if (processed[i] > 1)
            ++dups;
    }
    const std::uint64_t missing = kN - done;
    const std::uint64_t skipped = ch.tx_control().consumer_stats.inflight_skipped.Get();
    std::printf("  consumer kill: N=%llu kills=%llu processed=%llu missing=%llu inflight_skipped=%llu\n",
                (unsigned long long)kN,
                (unsigned long long)kills,
                (unsigned long long)done,
                (unsigned long long)missing,
                (unsigned long long)skipped);
    CHECK(dups == 0);                  // 没有重复处理：这就是"至多一次"
    CHECK(missing <= kills);           // 每次被杀至多丢一条（正在处理的那条）
    CHECK(skipped >= missing);         // 丢掉的都是被"跳过"的那条
    CHECK(skipped <= kills);           // 每次被杀至多跳过一条
    CHECK(kills >= 20 && skipped > 0); // 确实杀了很多次，且跳过路径确实走到了
    CHECK(missing > 0);                // "标记了但没处理就被杀"的路径也确实走到了：没有它，上面几条断言是空转
    CHECK(ch.tx_control().read_idx.load() == ch.tx_control().write_idx.load()); // 读空
}

// ---- 4. 生产者被 kill -9：批量发布是原子的，看不到半批；接手方接着写 ----

struct ProducerKillShared
{
    std::atomic<std::uint64_t> next_batch; // 已确认发布的批数（在 Commit 之后才加一）
    std::atomic<bool> finished;
};

struct BatchRecord
{
    std::uint64_t batch;
    std::uint64_t idx;
    std::uint64_t check;
};
constexpr unsigned kBatchSize = 8;

std::uint64_t BatchCheck(std::uint64_t batch, std::uint64_t idx)
{
    return (batch * 0x9E3779B97F4A7C15ULL) ^ (idx * 0xC2B2AE3D27D4EB4FULL) ^ 0xABCDEF;
}

void TestProducerKilled()
{
#ifdef NDEBUG
    constexpr std::uint64_t kBatches = 60000;
#else
    constexpr std::uint64_t kBatches = 20000;
#endif
    ShmChannel ch = MakeChannel();
    auto* sh = SharedAlloc<ProducerKillShared>(1);

    auto producer_body = [&]
    {
        RingProducer p = ch.TxProducer(); // 接着共享内存里已发布的 write_idx 往下写
        CHECK(!p.corrupt());
        for (;;)
        {
            const std::uint64_t bid = sh->next_batch.load(std::memory_order_acquire);
            if (bid >= kBatches)
                break;
            for (unsigned idx = 0; idx < kBatchSize; ++idx)
            {
                // Full 时不能 Commit（会把半批发布出去）：已发布的部分消费者会读走，空间自然会腾出来。
                auto r = p.TryReserve(sizeof(BatchRecord));
                while (r.status == ReserveStatus::kFull)
                {
                    ::sched_yield();
                    r = p.TryReserve(sizeof(BatchRecord));
                }
                CHECK(r.status == ReserveStatus::kOk);
                const BatchRecord rec{bid, idx, BatchCheck(bid, idx)};
                std::memcpy(r.payload.data(), &rec, sizeof(rec));
                if (bid % 16 == 0 && idx == 4)
                    ::usleep(40); // 拉宽"批没发完"的窗口
            }
            p.Commit();
            if (bid % 16 == 1)
                ::usleep(30); // Commit 之后、确认之前：被杀在这里，接手方会把这一批再发一遍（重复），但一定是完整的一批
            sh->next_batch.store(bid + 1, std::memory_order_release);
        }
        sh->finished.store(true, std::memory_order_release);
    };

    std::mt19937_64 rng(2468);
    pid_t prod = Spawn(producer_body);
    std::uint64_t kills = 0, dup_batches = 0;
    auto next_kill = Clock::now() + std::chrono::microseconds(500 + rng() % 6000);

    RingConsumer c = ch.TxConsumer();
    unsigned expect_idx = 0;
    std::uint64_t cur_bid = 0, last_bid = 0, groups = 0;
    bool have_last = false, producer_alive = true;
    for (;;)
    {
        RecordView v;
        bool progressed = false;
        while (c.Peek(v) == PeekStatus::kOk)
        {
            progressed = true;
            CHECK(v.len == sizeof(BatchRecord));
            BatchRecord rec;
            std::memcpy(&rec, v.payload, sizeof(rec));
            CHECK(rec.check == BatchCheck(rec.batch, rec.idx)); // 不会有半条记录
            if (expect_idx == 0)
            {
                // 新的一批：序号只能是上一批的同一个（重复）或下一个；第一批必须是 0。没有空洞。
                if (!have_last)
                    CHECK(rec.batch == 0);
                else
                {
                    CHECK(rec.batch == last_bid || rec.batch == last_bid + 1);
                    if (rec.batch == last_bid)
                        ++dup_batches;
                }
                cur_bid = rec.batch;
            }
            // 批内原子：同一批的 8 条连续出现、同一个批号、下标依次递增；半批会立刻让这里对不上。
            CHECK(rec.batch == cur_bid && rec.idx == expect_idx);
            if (++expect_idx == kBatchSize)
            {
                expect_idx = 0;
                last_bid = cur_bid;
                have_last = true;
                ++groups;
            }
            c.Pop();
        }
        if (producer_alive)
        {
            int st = 0;
            if (::waitpid(prod, &st, WNOHANG) == prod)
            {
                CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0); // 自己退出只能是全部发完
                producer_alive = false;
            }
            else if (Clock::now() >= next_kill && !sh->finished.load(std::memory_order_acquire))
            {
                if (KillAndReap(prod))
                {
                    ++kills;
                    CHECK(kills < 5000);
                    prod = Spawn(producer_body);
                    next_kill = Clock::now() + std::chrono::microseconds(500 + rng() % 6000);
                }
                else
                    producer_alive = false; // kill 到达之前它刚好发完退出了
            }
        }
        if (!producer_alive && !progressed && c.Peek(v) == PeekStatus::kEmpty)
            break;
        if (!progressed)
            ::sched_yield();
    }
    CHECK(expect_idx == 0); // 结束时没有半批
    CHECK(have_last && last_bid == kBatches - 1);
    std::printf("  producer kill: batches=%llu kills=%llu 重复的批=%llu groups=%llu\n",
                (unsigned long long)kBatches,
                (unsigned long long)kills,
                (unsigned long long)dup_batches,
                (unsigned long long)groups);
    CHECK(kills >= 20);
    CHECK(dup_batches <= kills); // 每次被杀至多多发一批
}

// ---- 5. 接手：App 重启式的角色更替（旧进程消失，新进程用 Attach 接着读）----

void TestConsumerTakeover()
{
    ShmChannel ch = MakeChannel();
    RingProducer p = ch.TxProducer();
    constexpr std::uint64_t kN = 1000;
    for (std::uint64_t i = 0; i < kN; ++i)
    {
        auto r = p.TryReserve(40);
        CHECK(r.status == ReserveStatus::kOk);
        FillPayload(r.payload, i);
    }
    p.Commit();

    // 第一任消费者读走前 400 条然后退出（没有任何清理）；第二任用 Attach 接手，从游标处继续，不丢不重。
    ExpectExit0(Spawn(
                    [&]
                    {
                        auto mine = AttachDup(ch);
                        CHECK(mine.has_value());
                        RingConsumer c = mine->TxConsumer();
                        for (std::uint64_t i = 0; i < 400; ++i)
                        {
                            RecordView v;
                            CHECK(c.Peek(v) == PeekStatus::kOk);
                            std::uint64_t seq = 0;
                            CHECK(CheckPayload(v.payload, v.len, &seq) && seq == i);
                            c.Pop();
                        }
                    }),
                "first consumer");
    ExpectExit0(Spawn(
                    [&]
                    {
                        auto mine = AttachDup(ch);
                        CHECK(mine.has_value());
                        RingConsumer c = mine->TxConsumer();
                        CHECK(c.RecoverFromCrash() == RecoverStatus::kClean);
                        for (std::uint64_t i = 400; i < kN; ++i)
                        {
                            RecordView v;
                            CHECK(c.Peek(v) == PeekStatus::kOk);
                            std::uint64_t seq = 0;
                            CHECK(CheckPayload(v.payload, v.len, &seq) && seq == i);
                            c.Pop();
                        }
                        RecordView v;
                        CHECK(c.Peek(v) == PeekStatus::kEmpty);
                    }),
                "second consumer");
}

// ---- 6. 封印：没有封印的对照用例要能复现 SIGBUS，证明封印确实必要 ----

bool DiedOfBusOrSanitizer(int st)
{
    // 普通构建：被 SIGBUS 杀死。sanitizer 构建：运行时接管了 SIGBUS，打印报告后以非零码退出。
    return (WIFSIGNALED(st) && WTERMSIG(st) == SIGBUS) || (WIFEXITED(st) && WEXITSTATUS(st) != 0);
}

void TestSigbusWithoutSeal()
{
    const long page = ::sysconf(_SC_PAGESIZE);
    const int st = WaitStatus(Spawn(
        [&]
        {
            auto fd = CreateMemFd("sigbus-demo", 2 * page);
            CHECK(fd.has_value());
            auto* p = static_cast<volatile char*>(
                ::mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_SHARED, fd->get(), 0));
            CHECK(p != MAP_FAILED);
            p[0] = 1;
            p[page] = 2;
            CHECK(::ftruncate(fd->get(), page) == 0); // 没有封印：对端可以把它缩小
            p[page] = 3;                              // 被截掉的页：SIGBUS
        }));
    std::printf("  无封印的对照：子进程 status=0x%x\n", st);
    CHECK(DiedOfBusOrSanitizer(st));

    // 封印之后同样的操作：缩小被拒绝，两页都还能访问。
    ExpectExit0(Spawn(
                    [&]
                    {
                        auto fd = CreateMemFd("sigbus-sealed", 2 * page);
                        CHECK(fd.has_value());
                        CHECK(SealMemFd(fd->get()) == 0);
                        auto* p = static_cast<volatile char*>(
                            ::mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_SHARED, fd->get(), 0));
                        CHECK(p != MAP_FAILED);
                        CHECK(::ftruncate(fd->get(), page) != 0);
                        p[0] = 1;
                        p[page] = 2;
                    }),
                "sealed");
}

} // namespace

int main()
{
    Arm(220);
    TestCrossProcessStream();
    RunPingPong(100000, /*spin=*/2000, "spin");
    RunPingPong(5000, /*spin=*/0, "always-sleep"); // 每条都走睡眠与唤醒：大量内核唤醒，丢一次就会超时
    TestConsumerKilled();
    TestProducerKilled();
    TestConsumerTakeover();
    TestSigbusWithoutSeal();
    std::puts("shm_multiprocess_test: OK");
    return 0;
}
