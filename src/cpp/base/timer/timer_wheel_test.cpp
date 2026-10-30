// TimerWheel 的语义用例（与参考实现的逐步对照在 timer_wheel_model_test.cpp）：
// 触发时刻与精度、同 tick 的顺序、取消 / 重置 / 句柄失效、重复定时器的两种补偿策略、回调里的各种操作、回调抛异常、
// 超出轮范围的延迟、空转跳过、节点不泄漏、NextDeadlineMs。无测试框架依赖：失败即 abort。

#include "timer_wheel.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace breeze;
using Repeat = TimerWheel::Repeat;
using TimerId = TimerWheel::TimerId;

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

constexpr std::int64_t kBase = 1000000; // 任意的基准毫秒数

// 把 wheel 的基准定在 kBase，之后用相对毫秒推进。
struct Fixture
{
    explicit Fixture(std::int64_t tick_ms = 1)
        : wheel(tick_ms)
    {
        wheel.Advance(kBase);
    }
    std::size_t At(std::int64_t rel_ms) { return wheel.Advance(kBase + rel_ms); }
    TimerWheel wheel;
};

void TestFiresAtTheRightTick()
{
    Fixture f;
    int n = 0;
    f.wheel.AddTimer(5, [&] { ++n; });
    CHECK(f.wheel.size() == 1);
    CHECK(f.At(4) == 0 && n == 0); // 5ms 的定时器 4ms 时不触发
    CHECK(f.At(5) == 1 && n == 1); // 5ms 整触发
    CHECK(f.wheel.size() == 0);
    CHECK(f.At(100) == 0 && n == 1); // 一次性：不会再来
}

void TestFirstAdvanceOnlySetsBase()
{
    TimerWheel w;
    int n = 0;
    w.AddTimer(10, [&] { ++n; }); // 基准还没定，计时从第一次 Advance 起算
    CHECK(!w.NextDeadlineMs().has_value());
    CHECK(w.Advance(500) == 0 && n == 0); // 第一次：只定基准
    CHECK(w.NextDeadlineMs() == std::optional<std::int64_t>(510));
    CHECK(w.Advance(509) == 0);
    CHECK(w.Advance(510) == 1 && n == 1);
}

void TestNonPositiveDelayMeansNextTick()
{
    for (const std::int64_t d : {std::int64_t{0}, std::int64_t{-1}, std::int64_t{-1000000}, INT64_MIN})
    {
        Fixture f;
        int n = 0;
        f.wheel.AddTimer(d, [&] { ++n; });
        CHECK(f.At(0) == 0); // 同一个 tick 内还不会触发
        CHECK(f.At(1) == 1 && n == 1);
    }
}

void TestTickPrecision()
{
    // tick = 10ms：延迟向上取整到 tick，到期 tick 一到就触发
    Fixture f(10);
    std::vector<int> fired;
    f.wheel.AddTimer(25, [&] { fired.push_back(25); }); // 3 个 tick
    f.wheel.AddTimer(30, [&] { fired.push_back(30); }); // 3 个 tick
    f.wheel.AddTimer(31, [&] { fired.push_back(31); }); // 4 个 tick
    f.wheel.AddTimer(1, [&] { fired.push_back(1); });   // 1 个 tick
    CHECK(f.At(9) == 0);
    CHECK(f.At(10) == 1 && fired == std::vector<int>{1});
    CHECK(f.At(29) == 0);
    CHECK(f.At(30) == 2 && (fired == std::vector<int>{1, 25, 30})); // 同一 tick：按创建顺序
    CHECK(f.At(39) == 0);
    CHECK(f.At(40) == 1 && fired.back() == 31);
    CHECK(f.wheel.tick_ms() == 10);
    CHECK(TimerWheel(0).tick_ms() == 1 && TimerWheel(-5).tick_ms() == 1);
}

void TestSameTickFiresInCreationOrder()
{
    Fixture f;
    std::vector<int> order;
    // A 先创建、但延迟长（要经过上层轮降级才到 level 0）；B 后创建、延迟短，直接放进 level 0。两者到期 tick 相同。
    f.wheel.AddTimer(1000, [&] { order.push_back(1); });
    f.At(900);
    f.wheel.AddTimer(100, [&] { order.push_back(2); });
    f.wheel.AddTimer(100, [&] { order.push_back(3); });
    f.At(1000);
    CHECK((order == std::vector<int>{1, 2, 3}));

    // 很多个同 tick 的
    Fixture g;
    std::vector<int> many;
    for (int i = 0; i < 500; ++i)
        g.wheel.AddTimer(50, [&many, i] { many.push_back(i); });
    g.At(50);
    CHECK(many.size() == 500);
    for (int i = 0; i < 500; ++i)
        CHECK(many[static_cast<std::size_t>(i)] == i);
}

void TestCancel()
{
    Fixture f;
    int n = 0;
    const TimerId a = f.wheel.AddTimer(10, [&] { ++n; });
    const TimerId b = f.wheel.AddTimer(10, [&] { ++n; });
    CHECK(f.wheel.IsActive(a) && f.wheel.size() == 2);
    CHECK(f.wheel.Cancel(a));
    CHECK(!f.wheel.IsActive(a) && f.wheel.size() == 1);
    CHECK(!f.wheel.Cancel(a)); // 句柄已失效
    CHECK(f.At(10) == 1 && n == 1);
    CHECK(!f.wheel.Cancel(b)); // 已经触发
    CHECK(!f.wheel.IsActive(b));
    CHECK(!f.wheel.Cancel(TimerWheel::kInvalidTimerId));
    CHECK(!f.wheel.Cancel(0xdeadbeefdeadbeefULL));
    CHECK(!f.wheel.IsActive(0xffffffffffffffffULL));
    CHECK(!f.wheel.Reset(TimerWheel::kInvalidTimerId, 5));
}

// 取消要立刻释放回调（捕获的资源），而不是拖到它本来的到期时间 —— 这是旧实现的内存增长问题。
void TestCancelReleasesCallbackImmediately()
{
    Fixture f;
    auto token = std::make_shared<int>(1);
    const TimerId id = f.wheel.AddTimer(3600 * 1000, [token] { (void)token; });
    CHECK(token.use_count() == 2);
    CHECK(f.wheel.Cancel(id));
    CHECK(token.use_count() == 1);

    // 清空、析构也一样
    auto token2 = std::make_shared<int>(2);
    f.wheel.AddTimer(1000, [token2] { (void)token2; });
    f.wheel.AddRepeating(1000, [token2] { (void)token2; });
    CHECK(token2.use_count() == 3);
    CHECK(f.wheel.Clear() == 2);
    CHECK(token2.use_count() == 1 && f.wheel.size() == 0);

    auto token3 = std::make_shared<int>(3);
    {
        TimerWheel w;
        w.AddTimer(1000, [token3] { (void)token3; });
        CHECK(token3.use_count() == 2);
    }
    CHECK(token3.use_count() == 1); // 析构时没触发的回调被销毁

    // 触发之后（一次性）也释放
    auto token4 = std::make_shared<int>(4);
    f.wheel.AddTimer(5, [token4] { (void)token4; });
    f.At(5);
    CHECK(token4.use_count() == 1);
}

// 句柄带代数：节点被复用后，旧句柄不能误伤新的定时器。
void TestStaleHandleDoesNotHitReusedSlot()
{
    Fixture f;
    int old_fired = 0, new_fired = 0;
    const TimerId old_id = f.wheel.AddTimer(10, [&] { ++old_fired; });
    CHECK(f.wheel.Cancel(old_id));
    const TimerId new_id = f.wheel.AddTimer(10, [&] { ++new_fired; }); // 复用同一个节点
    CHECK(new_id != old_id);
    CHECK(!f.wheel.Cancel(old_id)); // 旧句柄无效，不能取消新的
    CHECK(!f.wheel.Reset(old_id, 99));
    CHECK(!f.wheel.IsActive(old_id) && f.wheel.IsActive(new_id));
    CHECK(f.At(10) == 1 && old_fired == 0 && new_fired == 1);

    // 触发后复用同理
    const TimerId third = f.wheel.AddTimer(10, [&] { ++new_fired; });
    CHECK(!f.wheel.Cancel(new_id));
    CHECK(f.wheel.IsActive(third));
}

void TestReset()
{
    Fixture f;
    int n = 0;
    const TimerId id = f.wheel.AddTimer(100, [&] { ++n; });
    f.At(60);
    CHECK(f.wheel.Reset(id, 100)); // 从"现在"（tick 60）起重新计时：160
    CHECK(f.At(159) == 0);
    CHECK(f.At(160) == 1 && n == 1);
    CHECK(!f.wheel.Reset(id, 10)); // 已触发

    // 提前
    const TimerId e = f.wheel.AddTimer(1000, [&] { ++n; });
    CHECK(f.wheel.Reset(e, 5));
    CHECK(f.At(165) == 1 && n == 2);

    // 回调不动、不重新分配：连续重置很多次，节点数不变，只触发一次（"每收到一个包就重置空闲超时"）
    int timeouts = 0;
    const TimerId idle = f.wheel.AddTimer(30000, [&] { ++timeouts; });
    const std::size_t nodes = f.wheel.allocated_nodes();
    for (int i = 0; i < 20000; ++i)
    {
        f.At(165 + i / 10); // 时间在走
        CHECK(f.wheel.Reset(idle, 30000));
    }
    CHECK(f.wheel.allocated_nodes() == nodes);
    CHECK(timeouts == 0);
    f.At(165 + 20000 / 10 + 30000);
    CHECK(timeouts == 1);
}

void TestResetRepeatingKeepsInterval()
{
    Fixture f;
    std::vector<std::int64_t> at;
    std::int64_t now = 0;
    const TimerId id = f.wheel.AddRepeating(100, [&] { at.push_back(now); });
    now = 50;
    f.At(now);
    CHECK(f.wheel.Reset(id, 20)); // 下次 70，之后仍是每 100
    for (now = 51; now <= 400; ++now)
        f.At(now);
    CHECK((at == std::vector<std::int64_t>{70, 170, 270, 370}));
}

void TestOneShotInsideOwnCallback()
{
    Fixture f;
    TimerId self = 0;
    bool cancel_result = true, reset_result = true, active = true;
    self = f.wheel.AddTimer(5,
                            [&]
                            {
                                cancel_result = f.wheel.Cancel(self); // 一次性定时器已在回调里：没有"今后的触发"可阻止
                                reset_result = f.wheel.Reset(self, 10);
                                active = f.wheel.IsActive(self);
                            });
    f.At(5);
    CHECK(!cancel_result && !reset_result && !active);
    CHECK(f.At(1000) == 0); // 没有被 Reset 复活
}

void TestRepeating()
{
    Fixture f;
    int n = 0;
    const TimerId id = f.wheel.AddRepeating(10, [&] { ++n; });
    CHECK(f.wheel.size() == 1);
    for (int t = 1; t <= 100; ++t)
    {
        f.At(t);
        CHECK(n == t / 10);
    }
    CHECK(f.wheel.IsActive(id) && f.wheel.size() == 1);
    CHECK(f.wheel.Cancel(id));
    CHECK(f.At(200) == 0 && n == 10 && f.wheel.size() == 0);
}

void TestRepeatingCancelsItself()
{
    Fixture f;
    int n = 0;
    TimerId id = 0;
    id = f.wheel.AddRepeating(10,
                              [&]
                              {
                                  if (++n == 3)
                                  {
                                      CHECK(f.wheel.Cancel(id)); // 阻止了今后的触发
                                      CHECK(!f.wheel.IsActive(id));
                                      CHECK(!f.wheel.Cancel(id)); // 再取消：已经没有可阻止的了
                                      CHECK(f.wheel.size() == 0);
                                  }
                              });
    for (int t = 1; t <= 100; ++t) // 一步一步走（默认的 kSkipMissed 在一次大跳跃里只触发一次）
        f.At(t);
    CHECK(n == 3);
    CHECK(f.wheel.size() == 0 && !f.wheel.IsActive(id));
    CHECK(f.At(2000) == 0);
}

// 长停顿：kSkipMissed 只触发一次并保持相位，kCatchUp 逐个补
void TestRepeatPolicies()
{
    {
        Fixture f;
        std::vector<std::int64_t> at;
        std::int64_t now = 0;
        f.wheel.AddRepeating(100, [&] { at.push_back(now); }, Repeat::kSkipMissed);
        now = 1050; // 一口气过了 10 个周期
        CHECK(f.At(now) == 1);
        CHECK(at.size() == 1);
        // 相位不变：下次仍在 1100 的整数倍上
        now = 1099;
        CHECK(f.At(now) == 0);
        now = 1100;
        CHECK(f.At(now) == 1);
        now = 1150;
        CHECK(f.At(now) == 0);
        now = 1200;
        CHECK(f.At(now) == 1);
    }
    {
        Fixture f;
        int n = 0;
        f.wheel.AddRepeating(100, [&] { ++n; }, Repeat::kCatchUp);
        CHECK(f.At(1050) == 10 && n == 10); // 补上错过的 10 次
        CHECK(f.At(1099) == 0);
        CHECK(f.At(1100) == 1 && n == 11);
    }
    {
        // 默认策略是丢
        Fixture f;
        int n = 0;
        f.wheel.AddRepeating(100, [&] { ++n; });
        f.At(1050);
        CHECK(n == 1);
    }
}

void TestCallbackAddsTimersRelativeToProcessedTick()
{
    Fixture f;
    std::vector<std::int64_t> ticks;
    // 一次 Advance 跨过很多 tick：回调里再加 50ms 的定时器，相对的是"正在处理的那个 tick"，所以同一次 Advance
    // 里就能链式触发
    std::function<void()> chain;
    int depth = 0;
    chain = [&]
    {
        ticks.push_back(f.wheel.current_tick());
        if (++depth < 5)
            f.wheel.AddTimer(50, chain);
    };
    f.wheel.AddTimer(100, chain);
    CHECK(f.At(10000) == 5);
    CHECK((ticks == std::vector<std::int64_t>{100, 150, 200, 250, 300}));
}

void TestCallbackCancelsAndResetsOthersInTheSameTick()
{
    Fixture f;
    std::vector<int> fired;
    TimerId b = 0, c = 0, d = 0;
    f.wheel.AddTimer(10,
                     [&]
                     {
                         fired.push_back(1);
                         CHECK(f.wheel.Cancel(b));   // 同一 tick、还没轮到的：取消掉
                         CHECK(f.wheel.Reset(c, 5)); // 同一 tick、还没轮到的：改期
                         CHECK(f.wheel.IsActive(d));
                     });
    b = f.wheel.AddTimer(10, [&] { fired.push_back(2); });
    c = f.wheel.AddTimer(10, [&] { fired.push_back(3); });
    d = f.wheel.AddTimer(10, [&] { fired.push_back(4); });
    CHECK(f.wheel.size() == 4);
    CHECK(f.At(10) == 2 && (fired == std::vector<int>{1, 4}));
    CHECK(f.wheel.size() == 1);
    CHECK(f.At(14) == 0);
    CHECK(f.At(15) == 1 && fired.back() == 3); // c 被改到 15
    CHECK(f.wheel.size() == 0);
}

void TestClearInsideCallback()
{
    Fixture f;
    int a = 0, b = 0, r = 0;
    f.wheel.AddTimer(20, [&] { ++a; });
    f.wheel.AddTimer(30, [&] { ++b; });
    f.wheel.AddRepeating(
        10,
        [&]
        {
            ++r;
            if (r == 2)
                CHECK(f.wheel.Clear() >= 2); // 含正在跑的这个重复定时器
        },
        Repeat::kCatchUp); // 一次 Advance(1000) 里要让它在 10、20 各触发一次
    f.At(1000);
    CHECK(r == 2 && a == 1 && b == 0); // 20 的一次性比重复定时器先创建，同一 tick 里先触发；重复定时器在 20 清空全部
    CHECK(f.wheel.size() == 0);
    CHECK(f.At(2000) == 0);
}

void TestAdvanceIsNotReentrant()
{
    Fixture f;
    std::size_t inner = 99;
    int n = 0;
    f.wheel.AddTimer(5,
                     [&]
                     {
                         inner = f.wheel.Advance(kBase + 100000);
                         ++n;
                     });
    f.wheel.AddTimer(50, [&] { ++n; });
    CHECK(f.At(10) == 1);
    CHECK(inner == 0 && n == 1); // 回调里的 Advance 被忽略
    CHECK(f.At(50) == 1 && n == 2);
}

void TestTimeGoingBackwardsIsIgnored()
{
    Fixture f;
    int n = 0;
    f.wheel.AddTimer(100, [&] { ++n; });
    f.At(50);
    CHECK(f.At(10) == 0); // 回退：忽略，不倒流
    CHECK(f.wheel.Advance(kBase - 5000) == 0);
    CHECK(f.wheel.Advance(INT64_MIN) == 0); // now - base 会溢出，不能直接相减
    CHECK(f.At(99) == 0);
    CHECK(f.At(100) == 1 && n == 1); // 到期时刻不受那几次回退的影响
}

void TestEmptyCallbackRejected()
{
    Fixture f;
    CHECK(f.wheel.AddTimer(10, TimerWheel::Callback{}) == TimerWheel::kInvalidTimerId);
    CHECK(f.wheel.AddRepeating(10, TimerWheel::Callback{}) == TimerWheel::kInvalidTimerId);
    CHECK(f.wheel.size() == 0);
}

void TestMoveOnlyCallback()
{
    Fixture f;
    auto p = std::make_unique<int>(41);
    int got = 0;
    f.wheel.AddTimer(5, [&got, p = std::move(p)] { got = *p + 1; });
    f.At(5);
    CHECK(got == 42);
}

// 回调抛异常：时间轮保持一致，本 tick 剩下到期的留给下一次 Advance，异常传给调用方。
void TestCallbackThrows()
{
    Fixture f;
    std::vector<int> fired;
    f.wheel.AddTimer(10, [&] { fired.push_back(1); });
    f.wheel.AddTimer(10,
                     [&]
                     {
                         fired.push_back(2);
                         throw std::runtime_error("boom");
                     });
    f.wheel.AddTimer(10, [&] { fired.push_back(3); });
    f.wheel.AddTimer(10, [&] { fired.push_back(4); });
    bool threw = false;
    try
    {
        f.At(10);
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    CHECK(threw);
    CHECK((fired == std::vector<int>{1, 2})); // 抛的那个已经算触发过（一次性，已释放）；3、4 还在等
    CHECK(f.wheel.size() == 2);
    CHECK(f.wheel.NextDeadlineMs() == std::optional<std::int64_t>(kBase + 10)); // 有欠着的：现在就该醒
    CHECK(f.At(10) == 2 && (fired == std::vector<int>{1, 2, 3, 4}));            // 下一次 Advance 补上
    CHECK(f.wheel.size() == 0);
    CHECK(f.At(100) == 0);

    // 抛异常的重复定时器仍按周期重新排队
    int n = 0;
    f.wheel.AddRepeating(10,
                         [&]
                         {
                             if (++n == 1)
                                 throw std::runtime_error("once");
                         });
    try
    {
        f.At(110);
    }
    catch (const std::runtime_error&)
    {
    }
    CHECK(n == 1 && f.wheel.size() == 1);
    f.At(120);
    CHECK(n == 2);
}

// 延迟超过各层的范围（256 / 65536 / 2^24 / 2^32 个 tick），以及超出整个四层轮：到期 tick 前一刻不触发，到期那一刻触发。
void TestLongDelays()
{
    const std::int64_t delays[] = {255,
                                   256,
                                   257,
                                   65535,
                                   65536,
                                   65537,
                                   16777215,
                                   16777216,
                                   16777217,
                                   4294967295LL,
                                   4294967296LL,
                                   4294967297LL,
                                   8589934592LL,
                                   1LL << 40,
                                   (1LL << 40) + 12345};
    for (const std::int64_t d : delays)
    {
        Fixture f;
        int n = 0;
        f.wheel.AddTimer(d, [&] { ++n; });
        CHECK(f.wheel.size() == 1);
        CHECK(f.At(d - 1) == 0 && n == 0);
        CHECK(f.At(d) == 1 && n == 1);
        CHECK(f.wheel.size() == 0);
        CHECK(f.At(d * 2) == 0);
    }
    // 从一个不对齐的当前 tick 出发（当前 tick 不是 256 的整数倍）
    for (const std::int64_t d : delays)
    {
        Fixture f;
        f.At(1234567);
        int n = 0;
        f.wheel.AddTimer(d, [&] { ++n; });
        CHECK(f.At(1234567 + d - 1) == 0);
        CHECK(f.At(1234567 + d) == 1 && n == 1);
    }
}

void TestAbsurdDelaysAreClamped()
{
    Fixture f;
    int n = 0;
    const TimerId id = f.wheel.AddTimer(INT64_MAX, [&] { ++n; });
    CHECK(id != TimerWheel::kInvalidTimerId && f.wheel.IsActive(id));
    f.At(1000000);
    CHECK(n == 0 && f.wheel.size() == 1);
    CHECK(f.wheel.Reset(id, INT64_MAX));
    CHECK(f.wheel.Cancel(id));
    const TimerId r = f.wheel.AddRepeating(INT64_MAX, [&] { ++n; });
    CHECK(f.wheel.Cancel(r));
    // tick_ms 很大的时候向上取整不溢出
    TimerWheel big(INT64_MAX / 4);
    big.Advance(0);
    CHECK(big.AddTimer(INT64_MAX, [] {}) != TimerWheel::kInvalidTimerId);
    CHECK(big.NextDeadlineMs().has_value());
}

// now 大到离谱（INT64_MAX、基准是负数时 now - base 会溢出）：当前 tick
// 封顶，所有定时器照常触发，之后还能继续用，不出现溢出。
void TestHugeNowDoesNotOverflow()
{
    {
        Fixture f;
        int n = 0;
        f.wheel.AddTimer(10, [&] { ++n; });
        f.wheel.AddTimer(1LL << 40, [&] { ++n; });
        f.wheel.AddTimer(1LL << 44, [&] { ++n; }); // 超出轮的范围后每约 2^32 个 tick 降级一次：2^44 要 4096 次
        CHECK(f.wheel.Advance(INT64_MAX) == 3 && n == 3);
        CHECK(f.wheel.size() == 0);
        f.wheel.AddTimer(10, [&] { ++n; });
        CHECK(f.wheel.Advance(INT64_MAX) == 0); // 已经在封顶的 tick 上，新的 10 个 tick 之后的定时器到不了
        CHECK(f.wheel.size() == 1);
    }
    {
        TimerWheel w;
        w.Advance(INT64_MIN / 2); // 基准是很小的负数
        int n = 0;
        w.AddTimer(5, [&] { ++n; });
        CHECK(w.Advance(INT64_MAX / 2 + 10) == 1 && n == 1); // now - base 超出 int64
    }
}

// 没有事件的漫长时间一步跳过：推进 30 年不能逐 tick 空转。
void TestFastForward()
{
    Fixture f;
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(f.At(30LL * 365 * 86400 * 1000) == 0);
    int n = 0;
    f.wheel.AddTimer(5, [&] { ++n; });
    CHECK(f.At(30LL * 365 * 86400 * 1000 + 5) == 1 && n == 1); // 之后照常工作
    // 有一个很远的定时器：中间也是一步一步"有事才走"
    f.wheel.AddTimer(10LL * 365 * 86400 * 1000, [&] { ++n; });
    CHECK(f.At(60LL * 365 * 86400 * 1000) == 1 && n == 2);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 1000);
}

// 反复添加 / 取消不能让节点数增长（旧实现：取消的条目留在轮里直到原到期时间，内存随取消量线性增长）。
void TestNoNodeLeak()
{
    Fixture f;
    for (int i = 0; i < 200000; ++i)
    {
        const TimerId id = f.wheel.AddTimer(30000 + i % 5000, [] {});
        CHECK(f.wheel.Cancel(id));
    }
    CHECK(f.wheel.size() == 0);
    CHECK(f.wheel.allocated_nodes() == 1);

    // 同时存在 N 个：节点数就是 N，取消之后复用
    std::vector<TimerId> ids;
    for (int i = 0; i < 5000; ++i)
        ids.push_back(f.wheel.AddTimer(1000 + i, [] {}));
    CHECK(f.wheel.allocated_nodes() == 5000);
    for (const TimerId id : ids)
        CHECK(f.wheel.Cancel(id));
    for (int i = 0; i < 5000; ++i)
        f.wheel.AddTimer(1000 + i, [] {});
    CHECK(f.wheel.allocated_nodes() == 5000);

    // 触发后的节点也回收
    f.At(100000);
    CHECK(f.wheel.size() == 0);
    for (int i = 0; i < 5000; ++i)
        f.wheel.AddTimer(10, [] {});
    CHECK(f.wheel.allocated_nodes() == 5000);
}

void TestNextDeadlineMs()
{
    TimerWheel w;
    CHECK(!w.NextDeadlineMs().has_value()); // 没基准
    w.Advance(100);
    CHECK(!w.NextDeadlineMs().has_value()); // 没定时器

    w.AddTimer(30, [] {});
    CHECK(w.NextDeadlineMs() == std::optional<std::int64_t>(130)); // 第 0 层：精确
    w.AddTimer(10, [] {});
    CHECK(w.NextDeadlineMs() == std::optional<std::int64_t>(110));

    // 只有一个很远的：给出下界（上层轮降级的时刻），不晚于真正到期
    TimerWheel far;
    far.Advance(0);
    far.AddTimer(100000, [] {});
    const auto d = far.NextDeadlineMs();
    CHECK(d.has_value() && *d <= 100000 && *d > 0);
    CHECK(far.Advance(*d - 1) == 0);
    // 走到下界之后重新问：更近了，并且仍然不晚于到期时刻
    far.Advance(*d);
    const auto d2 = far.NextDeadlineMs();
    CHECK(d2.has_value() && *d2 >= *d && *d2 <= 100000);
    int fired = 0;
    for (std::int64_t now = *d2; far.size() > 0 && now <= 200000;)
    {
        fired += static_cast<int>(far.Advance(now));
        const auto nx = far.NextDeadlineMs();
        if (!nx)
            break;
        now = std::max(now + 1, *nx);
    }
    CHECK(fired == 1);

    // tick 精度：返回的是毫秒
    TimerWheel coarse(10);
    coarse.Advance(1000);
    coarse.AddTimer(35, [] {});
    CHECK(coarse.NextDeadlineMs() == std::optional<std::int64_t>(1040));
}

void TestClear()
{
    Fixture f;
    int n = 0;
    for (int i = 0; i < 100; ++i)
        f.wheel.AddTimer(10 + i * 1000, [&] { ++n; });
    f.wheel.AddRepeating(7, [&] { ++n; });
    CHECK(f.wheel.Clear() == 101);
    CHECK(f.wheel.size() == 0 && f.wheel.empty());
    CHECK(f.At(1000000) == 0 && n == 0);
    CHECK(f.wheel.Clear() == 0);
    // 清空之后可以继续用
    f.wheel.AddTimer(5, [&] { ++n; });
    f.At(1000005);
    CHECK(n == 1);
}

} // namespace

int main()
{
    TestFiresAtTheRightTick();
    TestFirstAdvanceOnlySetsBase();
    TestNonPositiveDelayMeansNextTick();
    TestTickPrecision();
    TestSameTickFiresInCreationOrder();
    TestCancel();
    TestCancelReleasesCallbackImmediately();
    TestStaleHandleDoesNotHitReusedSlot();
    TestReset();
    TestResetRepeatingKeepsInterval();
    TestOneShotInsideOwnCallback();
    TestRepeating();
    TestRepeatingCancelsItself();
    TestRepeatPolicies();
    TestCallbackAddsTimersRelativeToProcessedTick();
    TestCallbackCancelsAndResetsOthersInTheSameTick();
    TestClearInsideCallback();
    TestAdvanceIsNotReentrant();
    TestTimeGoingBackwardsIsIgnored();
    TestEmptyCallbackRejected();
    TestMoveOnlyCallback();
    TestCallbackThrows();
    TestLongDelays();
    TestAbsurdDelaysAreClamped();
    TestHugeNowDoesNotOverflow();
    TestFastForward();
    TestNoNodeLeak();
    TestNextDeadlineMs();
    TestClear();
    std::puts("timer_wheel_test: ok");
    return 0;
}
