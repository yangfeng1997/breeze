#include "now.h"

#include <chrono>

#include "time_util.h"

namespace breeze
{

namespace
{

std::int64_t SaturatingAdd(std::int64_t a, std::int64_t b) noexcept
{
    std::int64_t r = 0;
    if (__builtin_add_overflow(a, b, &r))
        return b > 0 ? std::numeric_limits<std::int64_t>::max() : std::numeric_limits<std::int64_t>::min() + 1;
    return r;
}

} // namespace

std::int64_t Now::WallMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::int64_t Now::SteadyNs() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::int64_t Now::SteadyMs() noexcept { return SteadyNs() / 1'000'000; }

std::int64_t Now::LiveMs() noexcept
{
    const std::int64_t frozen = State().frozen_ms.load(std::memory_order_relaxed);
    if (frozen != kUnset)
        return frozen;
    return SaturatingAdd(WallMs(), State().offset_ms.load(std::memory_order_relaxed));
}

void Now::Update() noexcept { State().cached_ms.store(LiveMs(), std::memory_order_relaxed); }

std::int64_t Now::Sec() noexcept { return TimeUtil::FloorDiv(Ms(), 1000); }

TimeString Now::Str() noexcept
{
    TimeString s;
    TimeUtil::Format(Sec(), TimezoneOffsetSec(), s.data, sizeof(s.data));
    return s;
}

void Now::SetOffsetMs(std::int64_t offset_ms) noexcept
{
    State().offset_ms.store(offset_ms, std::memory_order_relaxed);
    Update();
}

void Now::AddOffsetMs(std::int64_t delta_ms) noexcept
{
    State().offset_ms.fetch_add(delta_ms, std::memory_order_relaxed);
    Update();
}

void Now::FreezeMs(std::int64_t fixed_ms) noexcept
{
    State().frozen_ms.store(fixed_ms == kUnset ? kUnset + 1 : fixed_ms, std::memory_order_relaxed);
    Update();
}

void Now::Unfreeze() noexcept
{
    State().frozen_ms.store(kUnset, std::memory_order_relaxed);
    Update();
}

std::int32_t Now::TimezoneOffsetSec() noexcept
{
    const std::int64_t v = State().tz_offset_sec.load(std::memory_order_relaxed);
    if (v != kUnset)
        return static_cast<std::int32_t>(v);
    // 首次使用：采样本机时区。多个线程同时到这里也只是各算一次同样的值，后写的覆盖先写的，无害。
    const std::int32_t local = TimeUtil::LocalTimezoneOffset();
    std::int64_t expected = kUnset;
    State().tz_offset_sec.compare_exchange_strong(expected, local, std::memory_order_relaxed);
    return static_cast<std::int32_t>(State().tz_offset_sec.load(std::memory_order_relaxed));
}

void Now::SetTimezoneOffsetSec(std::int32_t offset_sec) noexcept
{
    State().tz_offset_sec.store(offset_sec, std::memory_order_relaxed);
}

void Now::ResetTimezoneToLocal() noexcept
{
    State().tz_offset_sec.store(TimeUtil::LocalTimezoneOffset(), std::memory_order_relaxed);
}

} // namespace breeze
