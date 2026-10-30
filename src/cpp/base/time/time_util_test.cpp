// TimeUtil：日历换算与 libc 全面对照、游戏日 / 周 / 月（含 reset_sec、负时间戳、各种时区）、格式化与解析、夏令时偏移。
// 已知答案由独立的 Python datetime 实现算出；随机用例对照 gmtime_r / timegm 与区间性质。无测试框架依赖：失败即 abort。

#include "time_util.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>
#include <string>

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

// ---- 已知答案（tools：/tmp 里的 Python 脚本，datetime + timedelta 独立实现） ----
struct DayCase
{
    std::int64_t ts;
    std::int32_t tz;
    std::int32_t reset;
    int week_start;
    std::int64_t day, week, month;
};
constexpr DayCase kDayCases[] = {
    {4586999627LL, 19800, 86399, 3, 4586956199LL, 4586783399LL, 4586178599LL},
    {-782397102LL, -34200, 1, 0, -782404199LL, -782922599LL, -783786599LL},
    {5653782490LL, -43200, 18000, 3, 5653731600LL, 5653645200LL, 5651485200LL},
    {830561963LL, -18000, 86399, 2, 830494799LL, 830321999LL, 828421199LL},
    {-950336162LL, 50400, 86399, 1, -950364001LL, -950364001LL, -952005601LL},
    {4183556871LL, 19800, 18000, 4, 4183486200LL, 4183399800LL, 4181153400LL},
    {173143464LL, 28800, 18000, 2, 173134800LL, 172789200LL, 170802000LL},
    {-1562671346LL, -34200, 86399, 3, -1562682601LL, -1562769001LL, -1564669801LL},
    {2101337933LL, -18000, 12600, 6, 2101278600LL, 2101278600LL, 2101192200LL},
    {4962825235LL, 19800, 1, 6, 4962796201LL, 4962537001LL, 4962191401LL},
    {1337036874LL, -34200, 12600, 6, 1337000400LL, 1336827600LL, 1335877200LL},
    {5003396557LL, 19800, 1, 2, 5003317801LL, 5003317801LL, 5001676201LL},
    {2838811284LL, -34200, 86399, 5, 2838792599LL, 2838533399LL, 2837582999LL},
    {1999136586LL, 50400, 12600, 6, 1999085400LL, 1998999000LL, 1998480600LL},
    {2693027145LL, 50400, 86399, 3, 2692951199LL, 2692519199LL, 2692778399LL},
    {4322288238LL, 19800, 86399, 5, 4322226599LL, 4322053799LL, 4320671399LL},
    {-731387509LL, 0, 1, 1, -731462399LL, -731462399LL, -733795199LL},
    {4386857664LL, 50400, 86399, 1, 4386823199LL, 4386391199LL, 4386477599LL},
    {65564964LL, 19800, 12600, 5, 65484000LL, 65397600LL, 63064800LL},
    {1661196326LL, 19800, 1, 2, 1661193001LL, 1661193001LL, 1659292201LL},
    {1706275491LL, 19800, 86399, 5, 1706207399LL, 1705688999LL, 1704133799LL},
    {1124226708LL, -18000, 0, 0, 1124168400LL, 1123995600LL, 1122872400LL},
    {1407879788LL, 19800, 12600, 4, 1407794400LL, 1407362400LL, 1406844000LL},
    {-1711515400LL, 50400, 12600, 3, -1711535400LL, -1711708200LL, -1712140200LL},
    {5068440072LL, 28800, 86399, 1, 5068367999LL, 5068022399LL, 5067503999LL},
    {2902164228LL, -43200, 12600, 6, 2902145400LL, 2902059000LL, 2900676600LL},
    {-765447556LL, 50400, 0, 5, -765468000LL, -765640800LL, -767973600LL},
    {2322402151LL, 19800, 12600, 5, 2322338400LL, 2321906400LL, 2321992800LL},
    {403841070LL, 28800, 86399, 6, 403804799LL, 403631999LL, 402335999LL},
    {5374155879LL, 19800, 0, 6, 5374146600LL, 5373801000LL, 5372505000LL},
    {3453921628LL, 28800, 18000, 0, 3453915600LL, 3453656400LL, 3452792400LL},
    {4798922988LL, 19800, 12600, 5, 4798908000LL, 4798562400LL, 4796661600LL},
    {2979750652LL, 28800, 86399, 4, 2979734399LL, 2979302399LL, 2979561599LL},
    {-1662461184LL, 19800, 1, 1, -1662528599LL, -1662874199LL, -1664774999LL},
    {115913931LL, -18000, 12600, 4, 115893000LL, 115547400LL, 115720200LL},
    {703223060LL, 19800, 18000, 1, 703207800LL, 703121400LL, 702084600LL},
    {-1282457204LL, -43200, 12600, 3, -1282465800LL, -1282811400LL, -1283416200LL},
    {700122247LL, 50400, 86399, 0, 700048799LL, 700048799LL, 699443999LL},
    {-1997161696LL, 0, 0, 5, -1997222400LL, -1997568000LL, -1998691200LL},
    {-1746381965LL, -43200, 18000, 3, -1746428400LL, -1746687600LL, -1748847600LL},
    {-698655289LL, 50400, 18000, 4, -698662800LL, -699181200LL, -699613200LL},
    {6925215486LL, 28800, 12600, 1, 6925203000LL, 6924684600LL, 6924079800LL},
    {1710540290LL, 19800, 0, 1, 1710527400LL, 1710095400LL, 1709231400LL},
    {-245506891LL, -43200, 12600, 5, -245579400LL, -246011400LL, -247307400LL},
    {6701311882LL, 28800, 18000, 5, 6701259600LL, 6701259600LL, 6700482000LL},
    {2520788207LL, 50400, 1, 4, 2520756001LL, 2520756001LL, 2519287201LL},
    {-1373062478LL, 28800, 86399, 2, -1373097601LL, -1373616001LL, -1375430401LL},
    {450959223LL, -18000, 1, 1, 450939601LL, 450939601LL, 449643601LL},
    {0LL, 0, 0, 1, 0LL, -259200LL, 0LL},
    {0LL, -18000, 0, 1, -68400LL, -241200LL, -2660400LL},
    {3600LL, -18000, 0, 1, -68400LL, -241200LL, -2660400LL},
    {-1LL, 0, 0, 1, -86400LL, -259200LL, -2678400LL},
    {-1LL, 28800, 18000, 0, -10800LL, -356400LL, -10800LL},
    {1700000000LL, 28800, 18000, 1, 1699995600LL, 1699822800LL, 1698786000LL},
};

// 编译期可算：公历换算是 constexpr
static_assert(TimeUtil::DaysFromCivil(1970, 1, 1) == 0);
static_assert(TimeUtil::DaysFromCivil(1969, 12, 31) == -1);
static_assert(TimeUtil::DaysFromCivil(2000, 1, 1) == 10957);
static_assert(TimeUtil::DaysFromCivil(2000, 3, 1) == 11017);
static_assert(TimeUtil::DaysFromCivil(1900, 3, 1) == -25508);
static_assert(TimeUtil::DaysFromCivil(1, 1, 1) == -719162);
static_assert(TimeUtil::DaysFromCivil(9999, 12, 31) == 2932896);
static_assert(TimeUtil::WeekdayOfDay(0) == static_cast<std::int64_t>(Weekday::Thursday));
static_assert(TimeUtil::WeekdayOfDay(-1) == static_cast<std::int64_t>(Weekday::Wednesday));
static_assert(TimeUtil::FloorDiv(-1, 86400) == -1 && TimeUtil::FloorDiv(-86400, 86400) == -1 &&
              TimeUtil::FloorDiv(-86401, 86400) == -2 && TimeUtil::FloorDiv(86399, 86400) == 0);
static_assert(TimeUtil::FloorMod(-1, 7) == 6 && TimeUtil::FloorMod(-7, 7) == 0 && TimeUtil::FloorMod(8, 7) == 1);
static_assert(TimeUtil::IsLeapYear(2000) && !TimeUtil::IsLeapYear(1900) && TimeUtil::IsLeapYear(2024) &&
              !TimeUtil::IsLeapYear(2023));
static_assert(TimeUtil::DaysInMonth(2024, 2) == 29 && TimeUtil::DaysInMonth(2023, 2) == 28 &&
              TimeUtil::DaysInMonth(2023, 12) == 31 && TimeUtil::DaysInMonth(2023, 4) == 30);

void TestDayWeekMonthKnownAnswers()
{
    for (const auto& c : kDayCases)
    {
        const auto ws = static_cast<Weekday>(c.week_start);
        CHECK(TimeUtil::DayBegin(c.ts, c.tz, c.reset) == c.day);
        CHECK(TimeUtil::WeekBegin(c.ts, c.tz, c.reset, ws) == c.week);
        CHECK(TimeUtil::MonthBegin(c.ts, c.tz, c.reset) == c.month);
    }
}

// 1970-01-01 之前、以及时区把本地时间推到 1970 之前：整数除法向零取整会错一天。
void TestNegativeTimestamps()
{
    // UTC-5 的 1970-01-01 01:00 UTC 是本地 1969-12-31 20:00，属于 12-31
    CHECK(TimeUtil::DayBegin(3600, -18000) == -68400);
    CHECK(TimeUtil::DayBegin(-1, 0) == -86400);
    CHECK(TimeUtil::DayBegin(-86400, 0) == -86400);
    CHECK(TimeUtil::DayBegin(-86401, 0) == -172800);
    CHECK(TimeUtil::IsSameDay(-1, -86400, 0));
    CHECK(!TimeUtil::IsSameDay(-1, 0, 0));
    CHECK(TimeUtil::DayIndex(-1, 0) == -1);
}

void TestResetHour()
{
    const std::int32_t tz = 8 * 3600, reset = 5 * 3600; // 东八区，每天凌晨 5 点刷新
    const auto at = [&](int y, int mo, int d, int h, int mi, int s)
    { return *TimeUtil::FromDateTime(DateTime{y, mo, d, h, mi, s}, tz); };

    // 04:59:59 还属于前一天，05:00:00 起算新的一天
    CHECK(!TimeUtil::IsSameDay(at(2024, 3, 10, 4, 59, 59), at(2024, 3, 10, 5, 0, 0), tz, reset));
    CHECK(TimeUtil::IsSameDay(at(2024, 3, 10, 5, 0, 0), at(2024, 3, 11, 4, 59, 59), tz, reset));
    CHECK(TimeUtil::IsSameDay(at(2024, 3, 9, 20, 0, 0), at(2024, 3, 10, 4, 59, 59), tz, reset));
    CHECK(TimeUtil::DayBegin(at(2024, 3, 10, 3, 0, 0), tz, reset) == at(2024, 3, 9, 5, 0, 0));
    CHECK(TimeUtil::DayBegin(at(2024, 3, 10, 7, 0, 0), tz, reset) == at(2024, 3, 10, 5, 0, 0));
    CHECK(TimeUtil::DayDiff(at(2024, 3, 10, 4, 0, 0), at(2024, 3, 10, 6, 0, 0), tz, reset) == 1);
    CHECK(TimeUtil::DayDiff(at(2024, 3, 10, 6, 0, 0), at(2024, 3, 10, 4, 0, 0), tz, reset) == -1);
    CHECK(TimeUtil::DaysBetween(at(2024, 3, 10, 6, 0, 0), at(2024, 3, 10, 4, 0, 0), tz, reset) == 1);
    CHECK(TimeUtil::DaysBetween(at(2024, 3, 1, 12, 0, 0), at(2024, 3, 31, 12, 0, 0), tz, reset) == 30);

    // 周一 03:00 在 5 点刷新的服里还属于上一周（周日所在的那周）
    const auto mon3 = at(2024, 1, 8, 3, 0, 0); // 2024-01-08 是周一
    const auto sun23 = at(2024, 1, 7, 23, 0, 0);
    const auto mon6 = at(2024, 1, 8, 6, 0, 0);
    CHECK(TimeUtil::IsSameWeek(mon3, sun23, tz, reset));
    CHECK(!TimeUtil::IsSameWeek(mon3, mon6, tz, reset));
    CHECK(TimeUtil::WeekBegin(mon6, tz, reset) == at(2024, 1, 8, 5, 0, 0));
    // 月：每月 1 号 03:00 仍属上个月
    CHECK(!TimeUtil::IsSameMonth(at(2024, 2, 1, 3, 0, 0), at(2024, 2, 1, 6, 0, 0), tz, reset));
    CHECK(TimeUtil::IsSameMonth(at(2024, 2, 1, 3, 0, 0), at(2024, 1, 31, 12, 0, 0), tz, reset));
    CHECK(TimeUtil::MonthBegin(at(2024, 2, 20, 12, 0, 0), tz, reset) == at(2024, 2, 1, 5, 0, 0));
}

void TestWeekStartAndMonth()
{
    // 2024-01-01 周一；01-06 周六；01-07 周日；01-08 周一
    const auto d = [](int day) { return *TimeUtil::FromDateTime(DateTime{2024, 1, day, 12, 0, 0}, 0); };
    CHECK(TimeUtil::IsSameWeek(d(1), d(7), 0)); // 周一起：周一 .. 周日同一周
    CHECK(!TimeUtil::IsSameWeek(d(7), d(8), 0));
    CHECK(!TimeUtil::IsSameWeek(d(6), d(7), 0, 0, Weekday::Sunday)); // 周日起：周日开始新的一周
    CHECK(TimeUtil::IsSameWeek(d(7), d(13), 0, 0, Weekday::Sunday));
    CHECK(TimeUtil::WeekBegin(d(3), 0) == *TimeUtil::FromDateTime(DateTime{2024, 1, 1}, 0));
    CHECK(TimeUtil::WeekBegin(d(3), 0, 0, Weekday::Wednesday) == *TimeUtil::FromDateTime(DateTime{2024, 1, 3}, 0));
    CHECK(TimeUtil::WeekBegin(d(2), 0, 0, Weekday::Wednesday) == *TimeUtil::FromDateTime(DateTime{2023, 12, 27}, 0));

    // 跨年的周、跨月
    const auto at = [](int y, int mo, int day) { return *TimeUtil::FromDateTime(DateTime{y, mo, day, 8, 0, 0}, 0); };
    CHECK(TimeUtil::IsSameWeek(at(2023, 12, 31), at(2024, 1, 1) - 86400 * 6 + 86400 * 0, 0) ==
          TimeUtil::IsSameWeek(at(2023, 12, 31), at(2023, 12, 26), 0));
    CHECK(TimeUtil::IsSameWeek(at(2023, 12, 31), at(2023, 12, 26), 0)); // 周二 .. 周日
    CHECK(!TimeUtil::IsSameWeek(at(2023, 12, 31), at(2024, 1, 1), 0));
    CHECK(TimeUtil::IsSameMonth(at(2024, 2, 1), at(2024, 2, 29), 0));
    CHECK(!TimeUtil::IsSameMonth(at(2024, 2, 29), at(2024, 3, 1), 0));
    CHECK(!TimeUtil::IsSameMonth(at(2023, 12, 31), at(2024, 12, 31), 0)); // 同月不同年
}

// 对随机输入验证区间性质（不依赖任何参考实现）。
void TestRandomizedProperties()
{
    std::mt19937_64 rng(2024);
    for (int i = 0; i < 200000; ++i)
    {
        const std::int64_t ts = static_cast<std::int64_t>(rng() % 20000000000ULL) - 5000000000LL; // 约 1811 .. 2604 年
        const std::int32_t tz = static_cast<std::int32_t>(rng() % (26 * 3600 + 1)) - 12 * 3600;
        const std::int32_t reset = static_cast<std::int32_t>(rng() % 86400);
        const auto ws = static_cast<Weekday>(rng() % 7);

        const std::int64_t db = TimeUtil::DayBegin(ts, tz, reset);
        CHECK(db <= ts && ts < db + 86400);
        CHECK(TimeUtil::FloorMod(db + tz, 86400) == reset); // 起点落在本地时间的 reset 秒处
        CHECK(TimeUtil::DayBegin(db, tz, reset) == db);     // 幂等
        CHECK(TimeUtil::DayBegin(db - 1, tz, reset) == db - 86400);
        CHECK(TimeUtil::IsSameDay(ts, db, tz, reset) && !TimeUtil::IsSameDay(ts, db + 86400, tz, reset));
        CHECK(TimeUtil::DayDiff(db, db + 86400 * 3, tz, reset) == 3);

        const std::int64_t wb = TimeUtil::WeekBegin(ts, tz, reset, ws);
        CHECK(wb <= ts && ts < wb + 7 * 86400);
        CHECK(wb == TimeUtil::DayBegin(wb, tz, reset)); // 起点同时是某个游戏日的起点
        CHECK(TimeUtil::WeekdayOfDay(TimeUtil::DayIndex(wb, tz, reset)) == static_cast<std::int64_t>(ws));
        CHECK(TimeUtil::WeekBegin(wb, tz, reset, ws) == wb);
        CHECK(TimeUtil::WeekBegin(wb - 1, tz, reset, ws) == wb - 7 * 86400);

        const std::int64_t mb = TimeUtil::MonthBegin(ts, tz, reset);
        CHECK(mb <= ts);
        CHECK(mb == TimeUtil::DayBegin(mb, tz, reset));
        std::int64_t y = 0;
        unsigned m = 0, d = 0;
        TimeUtil::CivilFromDays(TimeUtil::DayIndex(mb, tz, reset), y, m, d);
        CHECK(d == 1);
        const std::int64_t next = mb + TimeUtil::DaysInMonth(y, static_cast<int>(m)) * 86400LL;
        CHECK(ts < next);
        CHECK(TimeUtil::MonthBegin(next, tz, reset) == next);
        CHECK(TimeUtil::MonthBegin(next - 1, tz, reset) == mb);
    }
}

// 公历换算：逐日往返、相邻日期递增，并与 libc 的 gmtime_r / timegm 对照。
void TestCivilRoundTripEveryDay()
{
    std::int64_t py = 0;
    unsigned pm = 0, pd = 0;
    TimeUtil::CivilFromDays(-800000, py, pm, pd);
    for (std::int64_t day = -800000; day <= 800000; ++day)
    {
        std::int64_t y = 0;
        unsigned m = 0, d = 0;
        TimeUtil::CivilFromDays(day, y, m, d);
        CHECK(m >= 1 && m <= 12 && d >= 1 && d <= static_cast<unsigned>(TimeUtil::DaysInMonth(y, static_cast<int>(m))));
        CHECK(TimeUtil::DaysFromCivil(y, m, d) == day);
        if (day != -800000) // 与前一天相邻：要么同月日 +1，要么是下个月 1 号
        {
            const bool next_day = (y == py && m == pm && d == pd + 1);
            const bool next_month = (d == 1 && ((y == py && m == pm + 1) || (y == py + 1 && m == 1 && pm == 12)));
            CHECK(next_day || next_month);
        }
        py = y, pm = m, pd = d;
    }
}

void TestAgainstLibc()
{
    std::mt19937_64 rng(77);
    for (int i = 0; i < 300000; ++i)
    {
        const std::int64_t ts = static_cast<std::int64_t>(rng() % 100000000000ULL) - 10000000000LL;
        const std::int32_t tz = static_cast<std::int32_t>(rng() % (26 * 3600 + 1)) - 12 * 3600;
        const DateTime dt = TimeUtil::ToDateTime(ts, tz);

        const std::time_t t = static_cast<std::time_t>(ts + tz);
        std::tm g{};
        CHECK(::gmtime_r(&t, &g) != nullptr);
        CHECK(dt.year == g.tm_year + 1900 && dt.month == g.tm_mon + 1 && dt.day == g.tm_mday);
        CHECK(dt.hour == g.tm_hour && dt.minute == g.tm_min && dt.second == g.tm_sec);
        CHECK(static_cast<int>(dt.weekday) == g.tm_wday);

        // 反过来
        const auto back = TimeUtil::FromDateTime(dt, tz);
        CHECK(back.has_value() && *back == ts);
        CHECK(static_cast<std::int64_t>(::timegm(&g)) == ts + tz);
    }
}

void TestFromDateTimeValidation()
{
    const auto ok = [](int y, int mo, int d, int h = 0, int mi = 0, int s = 0)
    { return TimeUtil::FromDateTime(DateTime{y, mo, d, h, mi, s}, 0).has_value(); };
    CHECK(ok(2024, 2, 29) && ok(2000, 2, 29) && ok(2023, 12, 31, 23, 59, 59) && ok(1970, 1, 1));
    CHECK(!ok(2023, 2, 29) && !ok(1900, 2, 29) && !ok(2100, 2, 29));
    CHECK(!ok(2023, 4, 31) && !ok(2023, 6, 31) && !ok(2023, 9, 31) && !ok(2023, 11, 31));
    CHECK(!ok(2023, 0, 1) && !ok(2023, 13, 1) && !ok(2023, -1, 1));
    CHECK(!ok(2023, 1, 0) && !ok(2023, 1, 32) && !ok(2023, 1, -1));
    CHECK(!ok(2023, 1, 1, 24) && !ok(2023, 1, 1, -1) && !ok(2023, 1, 1, 0, 60) && !ok(2023, 1, 1, 0, 0, 60) &&
          !ok(2023, 1, 1, 0, -1, 0) && !ok(2023, 1, 1, 0, 0, -1));
    CHECK(TimeUtil::FromDateTime(DateTime{1970, 1, 1, 8, 0, 0}, 8 * 3600) == std::optional<std::int64_t>(0));
}

void TestFormat()
{
    CHECK(TimeUtil::Format(1700000000, 0) == "2023-11-14 22:13:20");
    CHECK(TimeUtil::Format(1700000000, 28800) == "2023-11-15 06:13:20");
    CHECK(TimeUtil::Format(-1, 0) == "1969-12-31 23:59:59");
    CHECK(TimeUtil::Format(0, -18000) == "1969-12-31 19:00:00");
    CHECK(TimeUtil::Format(951782400, 0) == "2000-02-29 00:00:00");
    CHECK(TimeUtil::Format(0, 0) == "1970-01-01 00:00:00");
    // 年份范围两端
    CHECK(TimeUtil::Format(*TimeUtil::FromDateTime(DateTime{1, 1, 1}, 0), 0) == "0001-01-01 00:00:00");
    CHECK(TimeUtil::Format(*TimeUtil::FromDateTime(DateTime{9999, 12, 31, 23, 59, 59}, 0), 0) == "9999-12-31 23:59:59");

    // 缓冲区：刚好 20 字节（19 + '\0'）够，19 字节不够（写空串、返回 0），0 字节什么也不写
    char buf[20];
    CHECK(TimeUtil::Format(1700000000, 0, buf, 20) == 19 && std::string(buf) == "2023-11-14 22:13:20");
    char small[19];
    small[0] = 'x';
    CHECK(TimeUtil::Format(1700000000, 0, small, 19) == 0 && small[0] == '\0');
    char untouched[1] = {'x'};
    CHECK(TimeUtil::Format(1700000000, 0, untouched, 0) == 0 && untouched[0] == 'x');
}

void TestIso8601()
{
    CHECK(TimeUtil::FormatIso8601(0) == "1970-01-01T00:00:00.000Z");
    CHECK(TimeUtil::FormatIso8601(1700000000123) == "2023-11-14T22:13:20.123Z");
    CHECK(TimeUtil::FormatIso8601(-1) == "1969-12-31T23:59:59.999Z"); // 负毫秒向下取整
    CHECK(TimeUtil::FormatIso8601(-1000) == "1969-12-31T23:59:59.000Z");
    CHECK(TimeUtil::FormatIso8601(-1001) == "1969-12-31T23:59:58.999Z");
    CHECK(TimeUtil::FormatIso8601(999) == "1970-01-01T00:00:00.999Z");
}

void TestParse()
{
    CHECK(TimeUtil::Parse("2023-11-14 22:13:20", 0) == std::optional<std::int64_t>(1700000000));
    CHECK(TimeUtil::Parse("2023-11-15 06:13:20", 28800) == std::optional<std::int64_t>(1700000000));
    CHECK(TimeUtil::Parse("2023-11-14", 0) == std::optional<std::int64_t>(1699920000)); // 只有日期：零点
    CHECK(TimeUtil::Parse("1969-12-31 23:59:59", 0) == std::optional<std::int64_t>(-1));
    CHECK(TimeUtil::Parse("2024-02-29 00:00:00", 0).has_value());

    for (const char* bad : {"",
                            "2023-11-14 22:13",
                            "2023-11-14T22:13:20",
                            "2023/11/14 22:13:20",
                            "2023-11-14  22:13:20",
                            " 2023-11-14 22:13:20",
                            "2023-11-14 22:13:20 ",
                            "2023-11-14 22:13:2a",
                            "2023-1-14 22:13:20",
                            "2023-02-29 00:00:00",
                            "2023-13-01 00:00:00",
                            "2023-00-01 00:00:00",
                            "2023-11-31 00:00:00",
                            "2023-11-14 24:00:00",
                            "2023-11-14 22:60:00",
                            "2023-11-14 22:13:60",
                            "+023-11-14",
                            "2023-11-1x",
                            "abcd-ef-gh",
                            "2023-11-14 22:13:20Z"})
    {
        CHECK(!TimeUtil::Parse(bad, 0).has_value());
    }

    // Format / Parse 往返
    std::mt19937_64 rng(5);
    for (int i = 0; i < 20000; ++i)
    {
        const std::int64_t ts = static_cast<std::int64_t>(rng() % 6000000000ULL) - 2000000000LL;
        const std::int32_t tz = static_cast<std::int32_t>(rng() % (26 * 3600 + 1)) - 12 * 3600;
        CHECK(TimeUtil::Parse(TimeUtil::Format(ts, tz), tz) == std::optional<std::int64_t>(ts));
    }
}

// 夏令时由 LocalTimezoneOffset(ts) 按时刻给出。用 POSIX 的 TZ 字符串，不依赖系统装了 tzdata。
// TZ 只能在没有别的线程时改：本进程到这里没有起过线程。
void TestLocalTimezoneOffset()
{
    ::setenv("TZ", "CST-8", 1);
    ::tzset();
    CHECK(TimeUtil::LocalTimezoneOffset(0) == 8 * 3600);
    CHECK(TimeUtil::LocalTimezoneOffset(1700000000) == 8 * 3600);

    ::setenv("TZ", "UTC0", 1);
    ::tzset();
    CHECK(TimeUtil::LocalTimezoneOffset(1700000000) == 0);

    // 美国东部：3 月第二个周日 2:00 起夏令时（UTC 07:00），11 月第一个周日 2:00 结束（UTC 06:00）
    ::setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
    ::tzset();
    const auto utc = [](int y, int mo, int d, int h, int mi, int s)
    { return *TimeUtil::FromDateTime(DateTime{y, mo, d, h, mi, s}, 0); };
    CHECK(TimeUtil::LocalTimezoneOffset(utc(2024, 1, 15, 12, 0, 0)) == -5 * 3600);
    CHECK(TimeUtil::LocalTimezoneOffset(utc(2024, 7, 15, 12, 0, 0)) == -4 * 3600);
    CHECK(TimeUtil::LocalTimezoneOffset(utc(2024, 3, 10, 6, 59, 59)) == -5 * 3600);
    CHECK(TimeUtil::LocalTimezoneOffset(utc(2024, 3, 10, 7, 0, 0)) == -4 * 3600);
    CHECK(TimeUtil::LocalTimezoneOffset(utc(2024, 11, 3, 5, 59, 59)) == -4 * 3600);
    CHECK(TimeUtil::LocalTimezoneOffset(utc(2024, 11, 3, 6, 0, 0)) == -5 * 3600);

    // 用"那一时刻"的偏移算日期边界：夏令时当天本地日期仍然是连续的
    const std::int32_t tz_before = TimeUtil::LocalTimezoneOffset(utc(2024, 3, 9, 12, 0, 0));
    const std::int32_t tz_after = TimeUtil::LocalTimezoneOffset(utc(2024, 3, 11, 12, 0, 0));
    CHECK(tz_before == -5 * 3600 && tz_after == -4 * 3600);
    CHECK(TimeUtil::DayDiff(utc(2024, 3, 9, 12, 0, 0), utc(2024, 3, 11, 12, 0, 0), tz_after) == 2);

    CHECK(TimeUtil::LocalTimezoneOffset() ==
          TimeUtil::LocalTimezoneOffset(static_cast<std::int64_t>(std::time(nullptr))));
}

} // namespace

int main()
{
    TestDayWeekMonthKnownAnswers();
    TestNegativeTimestamps();
    TestResetHour();
    TestWeekStartAndMonth();
    TestRandomizedProperties();
    TestCivilRoundTripEveryDay();
    TestAgainstLibc();
    TestFromDateTimeValidation();
    TestFormat();
    TestIso8601();
    TestParse();
    TestLocalTimezoneOffset();
    std::puts("time_util_test: ok");
    return 0;
}
