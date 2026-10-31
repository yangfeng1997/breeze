// PeriodicWindow：半开区间的边界、一次性 / 首尾相接 / 永久 / 无效窗口、负时间戳与接近前提上限的大数、
// 与暴力参考实现的穷举对照、与 TimeUtil / Now 配合。无测试框架依赖：失败即 abort。

#include "base/time/periodic_window.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <vector>

#include "base/time/now.h"
#include "base/time/time_util.h"

using namespace breeze;

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

using Opt = std::optional<std::int64_t>;

// 编译期可用：变异掉边界判断时，这几条会在构建阶段就失败。
constexpr PeriodicWindow kW{1000, 100, 500};
static_assert(!kW.Contains(999) && kW.Contains(1000) && kW.Contains(1099) && !kW.Contains(1100));
static_assert(kW.NextBegin(1000) == Opt{1500} && kW.NextBegin(999) == Opt{1000});
static_assert(kW.Index(1500) == Opt{1} && kW.CurrentEnd(1599) == Opt{1600});
// kForever：要大到当"永不关闭"，又要落在头文件注明的前提（< 2^60）之内，start + duration 才不会溢出。
static_assert(PeriodicWindow::kForever >= (std::int64_t{1} << 55) &&
              PeriodicWindow::kForever < (std::int64_t{1} << 60));

struct Expect
{
    std::int64_t now;
    Opt index, begin, end, next;
};

void CheckAll(const PeriodicWindow& w, const Expect& e)
{
    CHECK(w.Index(e.now) == e.index);
    CHECK(w.Contains(e.now) == e.index.has_value());
    CHECK(w.CurrentBegin(e.now) == e.begin);
    CHECK(w.CurrentEnd(e.now) == e.end);
    CHECK(w.NextBegin(e.now) == e.next);
}

// 起点那一秒是开的，终点那一秒已经关了（老 ActTimer::intime 用 <=，会多开一秒）。
void TestBoundaries()
{
    const PeriodicWindow w{1000, 100, 500};
    CHECK(w.Valid());
    const Expect cases[] = {
        {0, {}, {}, {}, 1000},
        {999, {}, {}, {}, 1000},
        {1000, 0, 1000, 1100, 1500}, // 恰好是起点：已经开了，下一期是 1500
        {1001, 0, 1000, 1100, 1500},
        {1099, 0, 1000, 1100, 1500},
        {1100, {}, {}, {}, 1500}, // 恰好是终点：关了
        {1499, {}, {}, {}, 1500},
        {1500, 1, 1500, 1600, 2000},
        {1599, 1, 1500, 1600, 2000},
        {1600, {}, {}, {}, 2000},
        {1000 + 500 * 1000000LL, 1000000, 1000 + 500 * 1000000LL, 1000 + 500 * 1000000LL + 100, 1000 + 500 * 1000001LL},
        {1000 + 500 * 1000000LL + 99,
         1000000,
         1000 + 500 * 1000000LL,
         1000 + 500 * 1000000LL + 100,
         1000 + 500 * 1000001LL},
        {1000 + 500 * 1000000LL + 100, {}, {}, {}, 1000 + 500 * 1000001LL},
    };
    for (const auto& e : cases)
        CheckAll(w, e);
}

void TestOneShot()
{
    const PeriodicWindow w{1000, 100, 0};
    CHECK(w.Valid());
    CheckAll(w, {999, {}, {}, {}, 1000});
    CheckAll(w, {1000, 0, 1000, 1100, {}}); // 开着时没有"下一期"
    CheckAll(w, {1099, 0, 1000, 1100, {}});
    CheckAll(w, {1100, {}, {}, {}, {}});
    CheckAll(w, {1000000000000LL, {}, {}, {}, {}});
}

// duration == interval：首尾相接，任何 >= start 的时刻都恰好落在一期里，终点那一秒归下一期。
void TestBackToBack()
{
    const PeriodicWindow w{0, 10, 10};
    CHECK(w.Valid());
    CheckAll(w, {-1, {}, {}, {}, 0});
    for (std::int64_t t = 0; t < 100; ++t)
        CheckAll(w, {t, t / 10, t / 10 * 10, t / 10 * 10 + 10, t / 10 * 10 + 10});
}

void TestForever()
{
    const PeriodicWindow w{5, PeriodicWindow::kForever, 0};
    CHECK(w.Valid());
    CheckAll(w, {4, {}, {}, {}, 5});
    CheckAll(w, {5, 0, 5, 5 + PeriodicWindow::kForever, {}});
    CheckAll(w, {std::int64_t{1} << 57, 0, 5, 5 + PeriodicWindow::kForever, {}});
    CHECK(!w.Contains(5 + PeriodicWindow::kForever)); // 终点同样是开区间
}

// 无效窗口：所有查询都当作"永远不开"，不崩、不溢出。
void TestInvalid()
{
    const PeriodicWindow bad[] = {
        {1000, 0, 500},    // 空窗口（老代码里 0 = 十年，这里不是）
        {1000, 0, 0},      //
        {1000, -5, 500},   // 负时长
        {1000, 100, -1},   // 负间隔
        {1000, 501, 500},  // 两期重叠
        {1000, 100, -500}, //
    };
    for (const auto& w : bad)
    {
        CHECK(!w.Valid());
        for (const std::int64_t t : {-1000LL, 0LL, 999LL, 1000LL, 1050LL, 1100LL, 1500LL, 1000000LL})
        {
            CHECK(!w.Contains(t));
            CHECK(!w.Index(t) && !w.CurrentBegin(t) && !w.CurrentEnd(t) && !w.NextBegin(t));
        }
    }
    // 刚好合法的边缘：duration == interval、interval == 0、duration == 1
    CHECK((PeriodicWindow{0, 500, 500}.Valid()));
    CHECK((PeriodicWindow{0, 1, 0}.Valid()));
    CHECK((PeriodicWindow{0, 1, 1}.Valid()));
    CHECK(!(PeriodicWindow{0, 2, 1}.Valid()));
}

// 1970 年之前的起点与时间戳；以及接近前提上限（±2^60）时不溢出（asan+ubsan 下会抓到有符号溢出）。
void TestNegativeAndLarge()
{
    const PeriodicWindow w{-100, 50, 100};
    CheckAll(w, {-101, {}, {}, {}, -100});
    CheckAll(w, {-100, 0, -100, -50, 0});
    CheckAll(w, {-51, 0, -100, -50, 0});
    CheckAll(w, {-50, {}, {}, {}, 0});
    CheckAll(w, {-1, {}, {}, {}, 0});
    CheckAll(w, {0, 1, 0, 50, 100});
    CheckAll(w, {49, 1, 0, 50, 100});
    CheckAll(w, {50, {}, {}, {}, 100});

    constexpr std::int64_t kLim = std::int64_t{1} << 60;
    const PeriodicWindow big{-kLim + 1, std::int64_t{1} << 58, std::int64_t{1} << 59};
    CHECK(big.Valid());
    const std::int64_t third = big.start + 3 * big.interval; // 第 3 期的起点
    CheckAll(big, {third, 3, third, third + big.duration, third + big.interval});
    CheckAll(big, {third + big.duration - 1, 3, third, third + big.duration, third + big.interval});
    CheckAll(big, {third + big.duration, {}, {}, {}, third + big.interval});
    CHECK(big.NextBegin(kLim - 1).has_value() && *big.NextBegin(kLim - 1) > kLim - 1);
    CHECK(!big.Contains(-kLim)); // 起点之前
    CHECK(big.NextBegin(-kLim) == Opt{big.start});

    // 间隔 1、跨 2^61 秒：每个时刻都在窗口里，序号 = now - start
    const PeriodicWindow dense{-kLim + 1, 1, 1};
    CheckAll(dense, {kLim - 1, 2 * kLim - 2, kLim - 1, kLim, kLim});
}

// 暴力参考：按定义把前 100 期一条条列出来，逐个比较。参数全部穷举（含无效的），一期都不会漏。
void TestAgainstBruteForce()
{
    constexpr int kWindows = 100; // now <= 60、start >= -5、interval >= 1 时最多 66 期，100 够
    std::int64_t cases = 0;
    for (std::int64_t start = -5; start <= 5; ++start)
        for (std::int64_t interval = -1; interval <= 12; ++interval)
            for (std::int64_t duration = -1; duration <= 14; ++duration)
            {
                const PeriodicWindow w{start, duration, interval};
                const bool valid = duration >= 1 && interval >= 0 && (interval == 0 || duration <= interval);
                CHECK(w.Valid() == valid);

                struct Win
                {
                    std::int64_t index, begin, end;
                };
                std::vector<Win> wins;
                if (valid)
                    for (int k = 0; k < (interval == 0 ? 1 : kWindows); ++k)
                        wins.push_back({k, start + k * interval, start + k * interval + duration});

                for (std::int64_t now = -12; now <= 60; ++now)
                {
                    Opt index, begin, end, next;
                    for (const auto& x : wins)
                    {
                        if (x.begin <= now && now < x.end)
                        {
                            index = x.index, begin = x.begin, end = x.end;
                            break; // 合法窗口互不重叠，至多一个命中
                        }
                    }
                    for (const auto& x : wins)
                    {
                        if (x.begin > now)
                        {
                            next = x.begin;
                            break;
                        }
                    }
                    CheckAll(w, {now, index, begin, end, next});
                    ++cases;
                }
            }
    CHECK(cases > 100000);
}

// 与 TimeUtil / Now 配合：每周六 20:00（东八区）开 2 小时；冻结 Now 之后窗口跟着变（GM 调时间生效）。
void TestWithTimeUtilAndNow()
{
    constexpr std::int32_t kTz = 8 * 3600;
    constexpr std::int64_t kWeek = 7 * 86400;
    const auto start = TimeUtil::Parse("2026-11-07 20:00:00", kTz);
    CHECK(start.has_value());
    CHECK(TimeUtil::ToDateTime(*start, kTz).weekday == Weekday::Saturday);

    const PeriodicWindow w{*start, 2 * 3600, kWeek};
    CHECK(w.Valid());

    // 第 3 期开始后 1 小时：开着；之后每一期的起点仍是周六 20:00
    Now::FreezeMs((*start + 3 * kWeek + 3600) * 1000);
    CHECK(Now::Sec() == *start + 3 * kWeek + 3600);
    CHECK(w.Contains(Now::Sec()));
    CHECK(w.Index(Now::Sec()) == Opt{3});
    const auto begin = w.CurrentBegin(Now::Sec());
    CHECK(begin.has_value());
    const DateTime dt = TimeUtil::ToDateTime(*begin, kTz);
    CHECK(dt.weekday == Weekday::Saturday && dt.hour == 20 && dt.minute == 0 && dt.second == 0);
    CHECK(w.CurrentEnd(Now::Sec()) == Opt{*start + 3 * kWeek + 2 * 3600});

    // 拨到窗口结束那一秒：关了，下一期是一周后
    Now::FreezeMs((*start + 3 * kWeek + 2 * 3600) * 1000);
    CHECK(!w.Contains(Now::Sec()));
    CHECK(w.NextBegin(Now::Sec()) == Opt{*start + 4 * kWeek});

    // 偏移：从冻结切回墙钟再加偏移，窗口判断跟着 Now::Sec() 走
    Now::Unfreeze();
    Now::SetOffsetMs(0);
    const std::int64_t real_now = Now::Sec();
    const std::int64_t to_next = *w.NextBegin(real_now) - real_now;
    Now::SetOffsetMs((to_next + 1) * 1000); // 拨到下一期开始之后 1 秒（NextBegin 恒 > now）
    CHECK(w.Contains(Now::Sec()));
    Now::SetOffsetMs(0);
}

} // namespace

int main()
{
    TestBoundaries();
    TestOneShot();
    TestBackToBack();
    TestForever();
    TestInvalid();
    TestNegativeAndLarge();
    TestAgainstBruteForce();
    TestWithTimeUtilAndNow();
    std::puts("periodic_window_test: ok");
    return 0;
}
