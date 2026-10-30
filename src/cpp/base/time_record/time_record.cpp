#include "time_record.h"

#include <format>
#include <utility>

#include "logger_macros.h"

namespace breeze
{

TimeRecord::TimeRecord(std::string name, std::chrono::microseconds slow_threshold, std::source_location loc)
    : start_(Clock::now())
    , name_(std::move(name))
    , slow_threshold_(slow_threshold)
    , loc_(loc)
    , enabled_(true)
{
}

TimeRecord::~TimeRecord()
{
    if (!enabled_)
        return;
    const std::int64_t us = ElapsedUs();
    const bool slow_mode = slow_threshold_.count() > 0;
    if (slow_mode && us < slow_threshold_.count())
        return;
    // 级别不够时 Log 自己会丢掉；这里只负责决定是 Info 还是 Warn
    CurrentLogger().Log(
        slow_mode ? Level::Warn : Level::Info, loc_, std::format("timerecord: name:{} elapsed {}us", name_, us));
}

} // namespace breeze
