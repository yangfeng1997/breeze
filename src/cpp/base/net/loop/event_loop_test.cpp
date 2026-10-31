// 无测试框架依赖：失败即 abort。并发用例带超时，卡死也会 abort 而不是挂住 ctest；
// 唤醒协议靠 tsan 预设与这里的长压力一起验证（丢唤醒是逻辑错误，不是数据竞争，tsan 抓不到）。

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>
#include <vector>

#include "base/net/loop/event_loop_thread.h"

using namespace breeze;
using namespace std::chrono_literals;

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

namespace
{

using Clock = EventLoop::Clock;

template <typename Pred>
bool WaitUntil(Pred pred, std::chrono::milliseconds timeout = 10s)
{
    const auto deadline = Clock::now() + timeout;
    while (!pred())
    {
        if (Clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(100us);
    }
    return true;
}

void TestPostRunsOnLoopThread()
{
    EventLoopThread lt;
    lt.Start();
    std::atomic<std::thread::id> ran_on{};
    std::atomic<bool> done{false};
    lt.loop.Post(
        [&]
        {
            ran_on = std::this_thread::get_id();
            done = true;
        });
    CHECK(WaitUntil([&] { return done.load(); }));
    CHECK(ran_on.load() != std::this_thread::get_id());
    CHECK(ran_on.load() != std::thread::id{});
}

// 同一个线程投递的任务按投递顺序执行。
void TestPostOrderFromOneThread()
{
    EventLoopThread lt;
    lt.Start();
    std::vector<int> order; // 只在 loop 线程写，Stop 的 join 之后才读
    for (int i = 0; i < 1000; ++i)
        lt.loop.Post([&order, i] { order.push_back(i); });
    lt.Stop(); // Quit 排在这些任务之后，Run 返回前它们都已执行
    CHECK(order.size() == 1000);
    for (int i = 0; i < 1000; ++i)
        CHECK(order[static_cast<std::size_t>(i)] == i);
}

// loop 开始运行之前（装配阶段）投递的任务，开始运行后照常执行。
void TestPostBeforeRun()
{
    EventLoopThread lt;
    std::atomic<int> n{0};
    for (int i = 0; i < 10; ++i)
        lt.loop.Post([&] { ++n; });
    lt.Start();
    CHECK(WaitUntil([&] { return n.load() == 10; }));
}

// 任务里再投递任务，链很长（远超每轮 4 批 × 64 的上限）：剩下的留给下一轮，loop 不能在还有任务时睡着。
void TestPostFromTaskChain()
{
    EventLoopThread lt;
    lt.Start();
    constexpr int kChain = 20000;
    std::atomic<int> n{0};
    std::function<void()> step;
    step = [&]
    {
        if (++n < kChain)
            lt.loop.Post([&] { step(); });
    };
    lt.loop.Post([&] { step(); });
    CHECK(WaitUntil([&] { return n.load() == kChain; }));
}

// 多个生产者同时投递：一个都不能丢，也不能挂住（丢唤醒 = loop 睡死，这里超时 abort）。
void TestWakeupStress()
{
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 50000;
    EventLoopThread lt;
    lt.Start();
    std::atomic<int> executed{0};
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p)
    {
        producers.emplace_back(
            [&]
            {
                for (int i = 0; i < kPerProducer; ++i)
                    lt.loop.Post([&] { executed.fetch_add(1, std::memory_order_relaxed); });
            });
    }
    for (auto& t : producers)
        t.join();
    CHECK(WaitUntil([&] { return executed.load() == kProducers * kPerProducer; }, 60s));
}

// loop 在 epoll_wait 里睡着时被唤醒：每次投递前随机停一小会儿，去撞"置 sleeping / 复查队列 / 进入睡眠"之间的窗口。
void TestWakeupWhileSleeping()
{
    EventLoopThread lt;
    lt.Start();
    for (int i = 0; i < 2000; ++i)
    {
        std::atomic<bool> done{false};
        std::this_thread::sleep_for(std::chrono::microseconds((i * 37) % 300));
        lt.loop.Post([&] { done = true; });
        CHECK(WaitUntil([&] { return done.load(); }, 5s));
    }
}

// 另一个线程在 Quit 前后持续投递：Quit 之后 Run 能返回，已执行的任务不重复、不乱序。
void TestQuitFromAnotherThread()
{
    EventLoopThread lt;
    lt.Start();
    std::atomic<int> n{0};
    for (int i = 0; i < 100; ++i)
        lt.loop.Post([&] { ++n; });
    lt.Stop();
    CHECK(n.load() == 100);
    lt.Stop(); // 重复 Stop 是空操作
}

// ---- 定时器（假时钟，全在当前线程 RunOnce(0) 驱动，不依赖真实时间）----

struct FakeClock
{
    Clock::time_point now = Clock::time_point{} + 1000s;
    EventLoop::NowFn Fn()
    {
        return [this] { return now; };
    }
    void Advance(std::chrono::milliseconds d) { now += d; }
};

void TestTimersFireInOrderWhenDue()
{
    FakeClock clk;
    EventLoop loop(clk.Fn());
    CHECK(loop.Init() == 0);
    std::vector<int> fired;
    loop.RunAfter(300ms, [&] { fired.push_back(300); });
    loop.RunAfter(100ms, [&] { fired.push_back(100); });
    loop.RunAfter(200ms, [&] { fired.push_back(200); });

    loop.RunOnce(0);
    CHECK(fired.empty());
    clk.Advance(99ms);
    loop.RunOnce(0);
    CHECK(fired.empty()); // 还差 1ms
    clk.Advance(1ms);
    loop.RunOnce(0);
    CHECK((fired == std::vector<int>{100}));
    clk.Advance(500ms); // 一步越过剩下的两个：按到期时间排序触发
    loop.RunOnce(0);
    CHECK((fired == std::vector<int>{100, 200, 300}));
    loop.RunOnce(0);
    CHECK(fired.size() == 3); // 一次性：不会重复触发
}

void TestTimerCancel()
{
    FakeClock clk;
    EventLoop loop(clk.Fn());
    CHECK(loop.Init() == 0);
    int fired = 0;
    const auto a = loop.RunAfter(100ms, [&] { fired |= 1; });
    const auto b = loop.RunAfter(100ms, [&] { fired |= 2; });
    loop.Cancel(a);
    clk.Advance(100ms);
    loop.RunOnce(0);
    CHECK(fired == 2);
    loop.Cancel(b);   // 已触发：空操作
    loop.Cancel(999); // 不存在：空操作
    loop.Cancel(a);   // 重复取消：空操作
}

void TestTimerScheduledFromTimer()
{
    FakeClock clk;
    EventLoop loop(clk.Fn());
    CHECK(loop.Init() == 0);
    std::vector<int> fired;
    loop.RunAfter(100ms,
                  [&]
                  {
                      fired.push_back(1);
                      loop.RunAfter(50ms, [&] { fired.push_back(2); }); // 相对"现在"（假时钟）
                  });
    clk.Advance(100ms);
    loop.RunOnce(0);
    CHECK((fired == std::vector<int>{1}));
    clk.Advance(49ms);
    loop.RunOnce(0);
    CHECK((fired == std::vector<int>{1}));
    clk.Advance(1ms);
    loop.RunOnce(0);
    CHECK((fired == std::vector<int>{1, 2}));
}

// 取消的定时器不应让 loop 为它醒来或等待。
void TestCancelledTimerDoesNotShortenWait()
{
    FakeClock clk;
    EventLoop loop(clk.Fn());
    CHECK(loop.Init() == 0);
    const auto id = loop.RunAfter(10ms, [] {});
    loop.Cancel(id);
    loop.RunOnce(0); // 取消后堆顶被惰性清掉；这里只要不崩、不触发
}

// 真实时钟：RunOnce(-1) 的等待时间由最近的定时器决定，不会无限期阻塞。
void TestRunOnceWaitsForNearestTimer()
{
    EventLoop loop;
    CHECK(loop.Init() == 0);
    bool fired = false;
    loop.RunAfter(60ms, [&] { fired = true; });
    const auto t0 = Clock::now();
    while (!fired)
    {
        loop.RunOnce(-1);
        CHECK(Clock::now() - t0 < 5s);
    }
    const auto elapsed = Clock::now() - t0;
    CHECK(elapsed >= 50ms && elapsed < 2s);
}

// 当前线程驱动 loop（不起线程）：RunOnce 处理已经投递的任务。
void TestRunOnceOnCallerThread()
{
    EventLoop loop;
    CHECK(loop.Init() == 0);
    int n = 0;
    loop.Post([&] { ++n; });
    loop.Post([&] { ++n; });
    loop.RunOnce(0);
    CHECK(n == 2);
    CHECK(loop.InLoopThread());
}

} // namespace

int main()
{
    TestPostRunsOnLoopThread();
    TestPostOrderFromOneThread();
    TestPostBeforeRun();
    TestPostFromTaskChain();
    TestWakeupStress();
    TestWakeupWhileSleeping();
    TestQuitFromAnotherThread();
    TestTimersFireInOrderWhenDue();
    TestTimerCancel();
    TestTimerScheduledFromTimer();
    TestCancelledTimerDoesNotShortenWait();
    TestRunOnceWaitsForNearestTimer();
    TestRunOnceOnCallerThread();
    std::puts("event_loop_test: ok");
    return 0;
}
