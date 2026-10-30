// Now：帧缓存、实时、GM 偏移与冻结、单调时钟、业务时区、并发读写。
// 全局状态，用例之间要自己还原（Reset）。第一个用例必须是"没调过 Update 的时候"。无测试框架依赖：失败即 abort。

#include "now.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "time_util.h"

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

constexpr std::int64_t kHour = 3600 * 1000;

void Reset()
{
    Now::Unfreeze();
    Now::SetOffsetMs(0);
}

std::int64_t Abs(std::int64_t v) { return v < 0 ? -v : v; }

// 必须最先跑：Update 从来没调过，缓存是空的 —— 读到的不能是 0，而是实时值。
void TestBeforeFirstUpdateFallsBackToLive()
{
    CHECK(Abs(Now::Ms() - Now::WallMs()) < 1000);
    CHECK(Abs(Now::Sec() - Now::WallMs() / 1000) <= 1);
}

void TestCacheIsStableWithinAFrame()
{
    Reset();
    Now::Update();
    const std::int64_t a = Now::Ms();
    std::this_thread::sleep_for(30ms);
    CHECK(Now::Ms() == a);          // 同一帧内多次取时间一致
    CHECK(Now::LiveMs() >= a + 25); // 实时值已经走了
    Now::Update();
    CHECK(Now::Ms() >= a + 25);
    CHECK(Abs(Now::Ms() - Now::WallMs()) < 1000);
    CHECK(Now::Sec() == Now::Ms() / 1000);
}

void TestOffset()
{
    Reset();
    Now::Update();
    const std::int64_t base = Now::Ms();
    Now::SetOffsetMs(kHour);
    // 改完立刻生效，不必再 Update
    CHECK(Abs(Now::Ms() - (base + kHour)) < 1000);
    CHECK(Now::offset_ms() == kHour);
    Now::AddOffsetMs(kHour);
    CHECK(Now::offset_ms() == 2 * kHour);
    CHECK(Abs(Now::Ms() - (base + 2 * kHour)) < 1000);
    Now::AddOffsetMs(-3 * kHour); // 回拨
    CHECK(Now::offset_ms() == -kHour);
    CHECK(Abs(Now::Ms() - (base - kHour)) < 1000);
    CHECK(Abs(Now::LiveMs() - (Now::WallMs() - kHour)) < 1000);
    // 物理墙钟不受影响
    CHECK(Abs(Now::WallMs() - base) < 1000);
    Reset();
    CHECK(Now::offset_ms() == 0 && Abs(Now::Ms() - base) < 1000);

    // 极端偏移：饱和，不回绕
    Now::SetOffsetMs(INT64_MAX);
    CHECK(Now::Ms() == INT64_MAX);
    Now::SetOffsetMs(INT64_MIN);
    CHECK(Now::Ms() < 0);
    Reset();
}

void TestFreeze()
{
    Reset();
    CHECK(!Now::frozen());
    Now::FreezeMs(1700000000123LL);
    CHECK(Now::frozen());
    CHECK(Now::Ms() == 1700000000123LL && Now::LiveMs() == 1700000000123LL);
    CHECK(Now::Sec() == 1700000000);
    std::this_thread::sleep_for(20ms);
    Now::Update();
    CHECK(Now::Ms() == 1700000000123LL);                // 冻结住了，时间不走
    CHECK(Abs(Now::WallMs() - 1700000000123LL) > 1000); // 物理墙钟当然不是这个

    // 0 与负数都是合法的冻结值（不能拿 0 当"未冻结"）
    Now::FreezeMs(0);
    CHECK(Now::frozen() && Now::Ms() == 0 && Now::LiveMs() == 0 && Now::Sec() == 0);
    Now::FreezeMs(-1);
    CHECK(Now::Ms() == -1 && Now::Sec() == -1); // 向下取整
    Now::FreezeMs(-1000);
    CHECK(Now::Sec() == -1);
    Now::FreezeMs(-1001);
    CHECK(Now::Sec() == -2);

    // 最小值本身被内部当作"未冻结"的标记：冻结在它上面不能悄悄变成"没冻结"，取最接近的值
    Now::FreezeMs(INT64_MIN);
    CHECK(Now::frozen() && Now::Ms() == INT64_MIN + 1);

    // 冻结优先于偏移
    Now::FreezeMs(5000);
    Now::SetOffsetMs(kHour);
    CHECK(Now::Ms() == 5000);

    Now::Unfreeze();
    CHECK(!Now::frozen());
    CHECK(Abs(Now::Ms() - (Now::WallMs() + kHour)) < 1000);
    Reset();
}

void TestSteadyIsUnaffectedBySimulation()
{
    Reset();
    const std::int64_t s0 = Now::SteadyMs();
    const std::int64_t n0 = Now::SteadyNs();
    Now::SetOffsetMs(1000 * kHour);
    Now::FreezeMs(42);
    std::this_thread::sleep_for(20ms);
    const std::int64_t s1 = Now::SteadyMs();
    CHECK(s1 >= s0 + 15 && s1 < s0 + 2000); // 只走了真实流逝的时间
    CHECK(Now::SteadyNs() > n0);
    CHECK(Now::SteadyMs() >= s1); // 单调
    Reset();
}

void TestStrAndTimezone()
{
    Reset();
    Now::FreezeMs(1700000000123LL);
    Now::SetTimezoneOffsetSec(0);
    CHECK(Now::TimezoneOffsetSec() == 0);
    CHECK(std::string(Now::Str().c_str()) == "2023-11-14 22:13:20");
    CHECK(Now::Str().view().size() == 19);
    Now::SetTimezoneOffsetSec(8 * 3600);
    CHECK(Now::TimezoneOffsetSec() == 8 * 3600);
    CHECK(std::string_view(Now::Str()) == "2023-11-15 06:13:20");
    Now::SetTimezoneOffsetSec(-5 * 3600);
    CHECK(std::string(Now::Str().c_str()) == "2023-11-14 17:13:20");
    // 同一秒内毫秒变化不影响字符串
    Now::FreezeMs(1700000000999LL);
    CHECK(std::string(Now::Str().c_str()) == "2023-11-14 17:13:20");
    Now::FreezeMs(1700000001000LL);
    CHECK(std::string(Now::Str().c_str()) == "2023-11-14 17:13:21");
    Reset();
}

// 与本机时区同步（TZ 环境变量只能在没有别的线程时改，这个用例在并发用例之前）。
void TestResetTimezoneToLocal()
{
    ::setenv("TZ", "CST-8", 1);
    ::tzset();
    Now::SetTimezoneOffsetSec(123);
    Now::ResetTimezoneToLocal();
    CHECK(Now::TimezoneOffsetSec() == 8 * 3600);
    ::setenv("TZ", "UTC0", 1);
    ::tzset();
    CHECK(Now::TimezoneOffsetSec() == 8 * 3600); // 不会自己跟着变
    Now::ResetTimezoneToLocal();
    CHECK(Now::TimezoneOffsetSec() == 0);
}

// 一个线程不停地改偏移 / 冻结 / Update，其余线程读：读到的字符串必须完整（19 个字符、数字与分隔符位置对），
// 时间戳必须落在写入过的取值范围里。字符串是按值返回的，不会像共享缓冲那样被写到一半读走。
void TestConcurrentReadersAndWriter()
{
    Reset();
    Now::SetTimezoneOffsetSec(0);
    std::atomic<bool> stop{false};
    std::atomic<long> reads{0};

    const auto well_formed = [](const TimeString& t)
    {
        const std::string_view v = t.view();
        if (v.size() != 19)
            return false;
        for (std::size_t i = 0; i < 19; ++i)
        {
            const char c = v[i];
            const bool sep = (i == 4 || i == 7)     ? c == '-'
                             : (i == 10)            ? c == ' '
                             : (i == 13 || i == 16) ? c == ':'
                                                    : false;
            const bool digit = c >= '0' && c <= '9';
            if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16 ? !sep : !digit)
                return false;
        }
        return true;
    };

    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i)
    {
        readers.emplace_back(
            [&]
            {
                while (!stop.load(std::memory_order_relaxed))
                {
                    const TimeString s = Now::Str();
                    CHECK(well_formed(s));
                    const std::int64_t ms = Now::Ms();
                    // 写线程只会设：冻结在 1e12 / 2e12，或偏移 0 / +1000 小时
                    CHECK(ms == 1000000000000LL || ms == 2000000000000LL || ms > 1000000000000LL);
                    reads.fetch_add(1, std::memory_order_relaxed);
                }
            });
    }
    std::thread writer(
        [&]
        {
            for (int i = 0; i < 20000; ++i)
            {
                switch (i % 5)
                {
                    case 0:
                        Now::FreezeMs(1000000000000LL);
                        break;
                    case 1:
                        Now::FreezeMs(2000000000000LL);
                        break;
                    case 2:
                        Now::Unfreeze();
                        Now::SetOffsetMs(1000 * kHour);
                        break;
                    case 3:
                        Now::AddOffsetMs(kHour);
                        Now::AddOffsetMs(-kHour);
                        break;
                    default:
                        Now::Update();
                        break;
                }
            }
            stop = true;
        });
    writer.join();
    for (auto& t : readers)
        t.join();
    CHECK(reads.load() > 0);
    Reset();
}

} // namespace

int main()
{
    TestBeforeFirstUpdateFallsBackToLive();
    TestCacheIsStableWithinAFrame();
    TestOffset();
    TestFreeze();
    TestSteadyIsUnaffectedBySimulation();
    TestStrAndTimezone();
    TestResetTimezoneToLocal();
    TestConcurrentReadersAndWriter();
    std::puts("now_test: ok");
    return 0;
}
