#include "time_util.h"

#include <cstdio>
#include <ctime>

namespace breeze
{

std::int32_t TimeUtil::LocalTimezoneOffset(std::int64_t ts_sec)
{
    const std::time_t t = static_cast<std::time_t>(ts_sec);
    std::tm local{};
    if (::localtime_r(&t, &local) == nullptr)
        return 0;
    return static_cast<std::int32_t>(local.tm_gmtoff);
}

std::int32_t TimeUtil::LocalTimezoneOffset()
{
    return LocalTimezoneOffset(static_cast<std::int64_t>(std::time(nullptr)));
}

DateTime TimeUtil::ToDateTime(std::int64_t ts_sec, std::int32_t tz_offset)
{
    const std::int64_t local = ts_sec + tz_offset;
    const std::int64_t day = FloorDiv(local, kSecondsPerDay);
    const auto sod = static_cast<std::int32_t>(local - day * kSecondsPerDay); // 一天内的秒数，0..86399

    std::int64_t y = 0;
    unsigned m = 0, d = 0;
    CivilFromDays(day, y, m, d);

    DateTime dt;
    dt.year = static_cast<std::int32_t>(y);
    dt.month = static_cast<std::int32_t>(m);
    dt.day = static_cast<std::int32_t>(d);
    dt.hour = sod / 3600;
    dt.minute = sod % 3600 / 60;
    dt.second = sod % 60;
    dt.weekday = static_cast<Weekday>(WeekdayOfDay(day));
    return dt;
}

std::optional<std::int64_t> TimeUtil::FromDateTime(const DateTime& dt, std::int32_t tz_offset)
{
    if (dt.month < 1 || dt.month > 12 || dt.day < 1 || dt.day > DaysInMonth(dt.year, dt.month) || dt.hour < 0 ||
        dt.hour > 23 || dt.minute < 0 || dt.minute > 59 || dt.second < 0 || dt.second > 59)
    {
        return std::nullopt;
    }
    const std::int64_t day = DaysFromCivil(dt.year, static_cast<unsigned>(dt.month), static_cast<unsigned>(dt.day));
    return day * kSecondsPerDay + dt.hour * 3600 + dt.minute * 60 + dt.second - tz_offset;
}

std::size_t TimeUtil::Format(std::int64_t ts_sec, std::int32_t tz_offset, char* buf, std::size_t cap) noexcept
{
    if (cap == 0)
        return 0;
    const DateTime dt = ToDateTime(ts_sec, tz_offset);
    const int n = std::snprintf(
        buf, cap, "%04d-%02d-%02d %02d:%02d:%02d", dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
    if (n < 0 || static_cast<std::size_t>(n) >= cap)
    {
        buf[0] = '\0';
        return 0;
    }
    return static_cast<std::size_t>(n);
}

std::string TimeUtil::Format(std::int64_t ts_sec, std::int32_t tz_offset)
{
    char buf[48]; // 年份最多 11 位（int32 的极值）也放得下
    const std::size_t n = Format(ts_sec, tz_offset, buf, sizeof(buf));
    return std::string(buf, n);
}

std::string TimeUtil::FormatIso8601(std::int64_t ts_ms)
{
    const std::int64_t sec = FloorDiv(ts_ms, 1000);
    const auto ms = static_cast<int>(ts_ms - sec * 1000);
    const DateTime dt = ToDateTime(sec, 0);
    char buf[64];
    const int n = std::snprintf(buf,
                                sizeof(buf),
                                "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                                dt.year,
                                dt.month,
                                dt.day,
                                dt.hour,
                                dt.minute,
                                dt.second,
                                ms);
    return n > 0 ? std::string(buf, static_cast<std::size_t>(n)) : std::string{};
}

namespace
{

// 固定宽度的十进制数字段，全是数字才成功。
bool ParseDigits(std::string_view s, int& out)
{
    int v = 0;
    for (const char c : s)
    {
        if (c < '0' || c > '9')
            return false;
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}

} // namespace

std::optional<std::int64_t> TimeUtil::Parse(std::string_view s, std::int32_t tz_offset)
{
    // "YYYY-MM-DD" 或 "YYYY-MM-DD HH:MM:SS"
    if (s.size() != 10 && s.size() != 19)
        return std::nullopt;
    DateTime dt;
    if (s[4] != '-' || s[7] != '-' || !ParseDigits(s.substr(0, 4), dt.year) || !ParseDigits(s.substr(5, 2), dt.month) ||
        !ParseDigits(s.substr(8, 2), dt.day))
    {
        return std::nullopt;
    }
    if (s.size() == 19)
    {
        if (s[10] != ' ' || s[13] != ':' || s[16] != ':' || !ParseDigits(s.substr(11, 2), dt.hour) ||
            !ParseDigits(s.substr(14, 2), dt.minute) || !ParseDigits(s.substr(17, 2), dt.second))
        {
            return std::nullopt;
        }
    }
    return FromDateTime(dt, tz_offset);
}

} // namespace breeze
