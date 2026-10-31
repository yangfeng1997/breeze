// 连接表：generation 校验、FIFO 复用、容量预留、回收清零、槽布局，以及与回收并发的 Find（tsan 下跑）。

#include "base/net/loop/connection_table.h"

#include <atomic>
#include <thread>
#include <vector>

#include "base/net/testing/net_test_util.h"

using namespace breeze;
using namespace breeze::testing;

namespace
{

using State = ConnectionTable::State;

void TestSlotLayout()
{
    static_assert(sizeof(ConnectionTable::Slot) == 64);
    static_assert(alignof(ConnectionTable::Slot) == 64);
    ConnectionTable t(8);
    // 相邻槽不在同一缓存行：别的线程更新一个槽的 pending_bytes 不会拖慢相邻槽。
    const auto a = reinterpret_cast<std::uintptr_t>(&t.slot(0));
    const auto b = reinterpret_cast<std::uintptr_t>(&t.slot(1));
    CHECK(a % 64 == 0 && b % 64 == 0 && b - a == 64);
}

void TestReserveHonorsCapacity()
{
    ConnectionTable t(2);
    CHECK(t.TryReserve());
    CHECK(t.TryReserve());
    CHECK(!t.TryReserve()); // 第三个超限
    CHECK(t.Live() == 2);   // 失败的预留不留痕
    t.Unreserve();
    CHECK(t.Live() == 1);
    CHECK(t.TryReserve());
}

void TestFindChecksGenerationAndState()
{
    ConnectionTable t(4);
    CHECK(t.TryReserve());
    const std::uint32_t s = t.Allocate(1000);
    ConnectionTable::Slot& slot = t.slot(s);
    CHECK(slot.generation.load() == 1); // 从 1 起：句柄 raw == 0 恒为无效
    CHECK(slot.high_water.load() == 1000);
    CHECK(t.Find(s, 1) == &slot);
    CHECK(t.Find(s, 2) == nullptr); // generation 不符
    CHECK(t.Find(s, 0) == nullptr);
    CHECK(t.Find(4, 1) == nullptr); // slot 越界
    CHECK(t.Find(UINT32_MAX, 1) == nullptr);

    slot.state.store(State::Disconnecting);
    CHECK(t.Find(s, 1) == nullptr); // 状态不是 Connected
    slot.state.store(State::Disconnected);
    CHECK(t.Find(s, 1) == nullptr);
}

void TestFifoReuseAndGenerationBump()
{
    ConnectionTable t(3);
    std::uint32_t got[3];
    for (auto& g : got)
    {
        CHECK(t.TryReserve());
        g = t.Allocate(0);
    }
    CHECK(got[0] == 0 && got[1] == 1 && got[2] == 2);

    t.Reclaim(1);
    t.Reclaim(0);
    CHECK(t.slot(1).generation.load() == 2); // 回收即 +1，旧句柄从此解析不到
    CHECK(t.Find(1, 1) == nullptr);
    CHECK(t.Live() == 1);

    // FIFO：先空出来的先被复用，拉长同一槽的复用间隔
    CHECK(t.TryReserve());
    CHECK(t.Allocate(0) == 1);
    CHECK(t.TryReserve());
    CHECK(t.Allocate(0) == 0);
    CHECK(t.Find(1, 2) != nullptr); // 新一代可解析
    CHECK(t.Find(1, 1) == nullptr); // 旧一代仍不可
}

void TestReclaimClearsPending()
{
    ConnectionTable t(2);
    CHECK(t.TryReserve());
    const std::uint32_t s = t.Allocate(0);
    t.slot(s).pending_bytes.store(12345);
    CHECK(t.PendingBytes(s) == 12345);
    t.Reclaim(s);
    CHECK(t.PendingBytes(s) == 0);
    CHECK(t.slot(s).state.load() == State::Free);
    CHECK(t.PendingBytes(999) == 0); // 越界不崩
}

// 回收线程反复分配 / 回收，另外几个线程不停 Find 并累加 pending_bytes：tsan 下不能有数据竞争；
// 每次回收恰好让一个槽的 generation + 1，所以总和可以精确断言。
void TestConcurrentFindWhileRecycling()
{
    constexpr std::uint32_t kCap = 8;
    constexpr int kCycles = 20000;
    ConnectionTable t(kCap);
    std::atomic<bool> stop{false};

    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i)
    {
        readers.emplace_back(
            [&]
            {
                std::uint32_t s = 0;
                while (!stop.load(std::memory_order_relaxed))
                {
                    s = (s + 1) % kCap;
                    const std::uint32_t gen = t.slot(s).generation.load(std::memory_order_acquire);
                    if (ConnectionTable::Slot* found = t.Find(s, gen))
                        found->pending_bytes.fetch_add(1, std::memory_order_relaxed);
                    (void)t.PendingBytes(s);
                }
            });
    }

    // 保持 kCap-1 个在用，剩一个循环：既覆盖 FIFO 复用，又不会因为满载而 Allocate 失败
    std::vector<std::uint32_t> live;
    for (std::uint32_t i = 0; i < kCap - 1; ++i)
    {
        CHECK(t.TryReserve());
        live.push_back(t.Allocate(1 << 20));
    }
    for (int i = 0; i < kCycles; ++i)
    {
        CHECK(t.TryReserve());
        const std::uint32_t s = t.Allocate(1 << 20);
        t.Reclaim(s);
    }
    stop.store(true);
    for (auto& th : readers)
        th.join();

    std::uint64_t bumps = 0;
    for (std::uint32_t i = 0; i < kCap; ++i)
        bumps += t.slot(i).generation.load() - 1;
    CHECK(bumps == static_cast<std::uint64_t>(kCycles));
}

} // namespace

int main()
{
    TestSlotLayout();
    TestReserveHonorsCapacity();
    TestFindChecksGenerationAndState();
    TestFifoReuseAndGenerationBump();
    TestReclaimClearsPending();
    TestConcurrentFindWhileRecycling();
    std::puts("connection_table_test: ok");
    return 0;
}
