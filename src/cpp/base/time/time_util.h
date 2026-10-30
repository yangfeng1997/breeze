// 无状态的日期 / 时间计算：全是纯函数，结果完全由入参决定，任意线程可调。时间戳单位秒（Unix 时间，UTC）。
//
// 时区一律用"固定偏移（秒，东为正）"传入，函数里不碰 TZ 环境变量；夏令时由调用方按那个时刻取偏移
// （LocalTimezoneOffset(ts)），或者像大多数游戏服那样在配置里写死一个区域偏移。
//
// "游戏日"：很多玩法每天凌晨 5 点而不是 0 点刷新。带 reset_sec 的函数把每天的起点放在本地时间 reset_sec 秒处
// （0 = 午夜，5*3600 = 凌晨 5 点）：凌晨 3 点属于前一个游戏日。周、月都按游戏日算（周一 03:00 在 5
// 点刷新的服里仍属上周）。
//
// 算法是 Howard Hinnant 的 civil-days 公式：整数运算、负时间戳（1970 年之前、或时区把本地时间推到 1970 之前）也正确，
// 不依赖 libc 的 gmtime / timegm。前提：时间戳的绝对值远小于 2^62（别拿 INT64_MAX 当"永不过期"传进来）。

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace breeze
{

// 与 struct tm::tm_wday 同一编号。
enum class Weekday : std::int32_t
{
    Sunday = 0,
    Monday,
    Tuesday,
    Wednesday,
    Thursday,
    Friday,
    Saturday,
};

struct DateTime
{
    std::int32_t year = 1970;
    std::int32_t month = 1; // 1..12
    std::int32_t day = 1;   // 1..31
    std::int32_t hour = 0;
    std::int32_t minute = 0;
    std::int32_t second = 0;
    Weekday weekday = Weekday::Thursday; // 只在 ToDateTime 的结果里有意义；FromDateTime 不读它
};

inline constexpr std::int64_t kSecondsPerDay = 86400;

class TimeUtil
{
public:
    // ---- 时区 ----

    // 本机时区在 ts 这一时刻的偏移（秒，东八区 = +28800），含夏令时。受 TZ 环境变量影响；内部走 localtime_r，会取一把
    // libc 的锁， 别放热路径，启动时取一次、或按需缓存。
    static std::int32_t LocalTimezoneOffset(std::int64_t ts_sec);
    static std::int32_t LocalTimezoneOffset(); // 当前时刻

    // ---- 日历换算 ----

    // 自 1970-01-01 起的天数 <-> 公历日期（proleptic Gregorian，可为负）。
    static constexpr std::int64_t DaysFromCivil(std::int64_t y, unsigned m, unsigned d) noexcept
    {
        y -= m <= 2;
        const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
        const auto yoe = static_cast<unsigned>(y - era * 400);
        const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
    }
    static constexpr void CivilFromDays(std::int64_t z, std::int64_t& y, unsigned& m, unsigned& d) noexcept
    {
        z += 719468;
        const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
        const auto doe = static_cast<unsigned>(z - era * 146097);
        const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        y = static_cast<std::int64_t>(yoe) + era * 400;
        const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const unsigned mp = (5 * doy + 2) / 153;
        d = doy - (153 * mp + 2) / 5 + 1;
        m = mp < 10 ? mp + 3 : mp - 9;
        y += m <= 2;
    }

    static constexpr bool IsLeapYear(std::int64_t y) noexcept { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
    static constexpr int DaysInMonth(std::int64_t year, int month) noexcept
    {
        constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        return (month == 2 && IsLeapYear(year)) ? 29 : kDays[month - 1];
    }

    // 向负无穷取整的除法 / 取模。C++ 的 / 向零取整，负数时差一个单位（DayBegin 在 1970 年之前、或 UTC-5 的 1970-01-01
    // 凌晨都会碰到）。
    static constexpr std::int64_t FloorDiv(std::int64_t a, std::int64_t b) noexcept
    {
        const std::int64_t q = a / b;
        return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
    }
    static constexpr std::int64_t FloorMod(std::int64_t a, std::int64_t b) noexcept { return a - FloorDiv(a, b) * b; }

    // 时间戳 -> 该时区的日历时间（含星期）。
    static DateTime ToDateTime(std::int64_t ts_sec, std::int32_t tz_offset);
    // 日历时间 -> 时间戳。字段越界（13 月、2 月 30 日、25 点、60 秒）返回 nullopt；weekday 字段被忽略。
    static std::optional<std::int64_t> FromDateTime(const DateTime& dt, std::int32_t tz_offset);

    // ---- 游戏日 / 周 / 月（reset_sec：每天的起点，本地时间的秒数，0 .. 86399） ----

    // 自 1970-01-01 起的游戏日序号（可为负）。
    static std::int64_t DayIndex(std::int64_t ts_sec, std::int32_t tz_offset, std::int32_t reset_sec = 0) noexcept
    {
        return FloorDiv(ts_sec + tz_offset - reset_sec, kSecondsPerDay);
    }
    // ts 所在游戏日的起点（时间戳）。reset_sec = 0 时就是当地 0 点。
    static std::int64_t DayBegin(std::int64_t ts_sec, std::int32_t tz_offset, std::int32_t reset_sec = 0) noexcept
    {
        return DayIndex(ts_sec, tz_offset, reset_sec) * kSecondsPerDay + reset_sec - tz_offset;
    }
    static bool IsSameDay(std::int64_t a, std::int64_t b, std::int32_t tz_offset, std::int32_t reset_sec = 0) noexcept
    {
        return DayIndex(a, tz_offset, reset_sec) == DayIndex(b, tz_offset, reset_sec);
    }
    // 从 from 到 to 隔了几个游戏日（有符号，to 在后为正）。同一游戏日为 0。
    static std::int64_t DayDiff(std::int64_t from,
                                std::int64_t to,
                                std::int32_t tz_offset,
                                std::int32_t reset_sec = 0) noexcept
    {
        return DayIndex(to, tz_offset, reset_sec) - DayIndex(from, tz_offset, reset_sec);
    }
    // DayDiff 的绝对值。
    static std::int64_t DaysBetween(std::int64_t a,
                                    std::int64_t b,
                                    std::int32_t tz_offset,
                                    std::int32_t reset_sec = 0) noexcept
    {
        const std::int64_t d = DayDiff(a, b, tz_offset, reset_sec);
        return d < 0 ? -d : d;
    }

    // ts 所在周的起点；week_start 指定一周从星期几开始（默认周一）。起点的时刻是该天的游戏日起点。
    static std::int64_t WeekBegin(std::int64_t ts_sec,
                                  std::int32_t tz_offset,
                                  std::int32_t reset_sec = 0,
                                  Weekday week_start = Weekday::Monday) noexcept
    {
        const std::int64_t day = DayIndex(ts_sec, tz_offset, reset_sec);
        const std::int64_t since_start = FloorMod(WeekdayOfDay(day) - static_cast<std::int64_t>(week_start), 7);
        return (day - since_start) * kSecondsPerDay + reset_sec - tz_offset;
    }
    static bool IsSameWeek(std::int64_t a,
                           std::int64_t b,
                           std::int32_t tz_offset,
                           std::int32_t reset_sec = 0,
                           Weekday week_start = Weekday::Monday) noexcept
    {
        return WeekBegin(a, tz_offset, reset_sec, week_start) == WeekBegin(b, tz_offset, reset_sec, week_start);
    }

    // ts 所在（游戏日意义下的）月份的 1 号起点。
    static std::int64_t MonthBegin(std::int64_t ts_sec, std::int32_t tz_offset, std::int32_t reset_sec = 0) noexcept
    {
        std::int64_t y = 0;
        unsigned m = 0, d = 0;
        CivilFromDays(DayIndex(ts_sec, tz_offset, reset_sec), y, m, d);
        return DaysFromCivil(y, m, 1) * kSecondsPerDay + reset_sec - tz_offset;
    }
    static bool IsSameMonth(std::int64_t a, std::int64_t b, std::int32_t tz_offset, std::int32_t reset_sec = 0) noexcept
    {
        return MonthBegin(a, tz_offset, reset_sec) == MonthBegin(b, tz_offset, reset_sec);
    }

    // 第 day 天（自 1970-01-01 起）是星期几。1970-01-01 是星期四。
    static constexpr std::int64_t WeekdayOfDay(std::int64_t day) noexcept { return FloorMod(day + 4, 7); }

    // ---- 格式化 / 解析 ----

    // "YYYY-MM-DD HH:MM:SS"（该时区的本地时间）。年份按 %04d 输出，1..9999 之外不保证好看但不会出错。
    static std::string Format(std::int64_t ts_sec, std::int32_t tz_offset);
    // 写进 buf（带 '\0'），返回写入的字符数（不含 '\0'）；cap 不够时写空串并返回 0。至少要 20 字节。
    static std::size_t Format(std::int64_t ts_sec, std::int32_t tz_offset, char* buf, std::size_t cap) noexcept;
    // UTC 的 ISO 8601："YYYY-MM-DDTHH:MM:SS.mmmZ"。毫秒时间戳为负时也正确。
    static std::string FormatIso8601(std::int64_t ts_ms);
    // 严格解析 "YYYY-MM-DD HH:MM:SS" 或 "YYYY-MM-DD"（零点），按 tz_offset 解释。多余字符、越界字段都失败。
    static std::optional<std::int64_t> Parse(std::string_view s, std::int32_t tz_offset);
};

} // namespace breeze
