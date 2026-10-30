// 进程级的时钟：帧缓存的"当前时间"、可模拟的逻辑时间（GM 快进 / 冻结）、单调时钟、业务时区。
//
// 主循环每帧调一次 Now::Update()，之后全帧读到的 Ms() / Sec() / Str() 都是同一个时刻：同一帧里多处取时间互相一致，
// 也省掉重复的系统调用。需要"此刻"而不是"本帧"时用 LiveMs()。
//
// 线程安全：全部状态是原子量。Update() 可以由任意线程调（通常只有主循环线程），Ms() 等可以在任意线程读，
// 不会读到撕裂的值；Str() 按值返回，不像"返回内部缓冲指针"那样会在读的时候被别的线程改写。
//
// 逻辑时间 = 物理墙钟 + 偏移，或者被冻结在某个值上。模拟只影响"墙钟意义"的时间（日期、活动开关、每日刷新）；
// SteadyMs() 不受影响，定时器 / 超时 / 耗时统计一律用它，GM 把时间拨一年也不会让一堆定时器同时到期。
//
// 业务时区是一个全进程的固定偏移（秒，东为正），默认取启动后第一次用到时本机的偏移。夏令时不会自动跟踪：
// 服务器一般固定在一个区域，建议由配置显式 SetTimezoneOffsetSec；本机时区变了可以 ResetTimezoneToLocal。
//
// 没调过 Update() 时 Ms() / Sec() 退化成 LiveMs()，而不是返回 0。

#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <string_view>

namespace breeze
{

// "YYYY-MM-DD HH:MM:SS"，按值返回的定长缓冲。
struct TimeString
{
    char data[20] = {};

    const char* c_str() const noexcept { return data; }
    std::string_view view() const noexcept { return {data}; }
    operator std::string_view() const noexcept { return view(); }
};

class Now
{
public:
    // ---- 帧缓存（读缓存，不查系统时钟） ----

    // 刷新缓存为当前的逻辑时间。
    static void Update() noexcept;
    static std::int64_t Ms() noexcept
    {
        const std::int64_t v = State().cached_ms.load(std::memory_order_relaxed);
        return v != kUnset ? v : LiveMs();
    }
    static std::int64_t Sec() noexcept;
    // 缓存时间按业务时区格式化。
    static TimeString Str() noexcept;

    // ---- 实时（每次都查系统时钟，有开销） ----

    // 逻辑时间：墙钟 + 偏移，冻结时是冻结值。
    static std::int64_t LiveMs() noexcept;
    // 物理墙钟，不含任何模拟。
    static std::int64_t WallMs() noexcept;
    // 单调时钟（自某个固定起点），不受系统时钟调整与模拟影响。
    static std::int64_t SteadyMs() noexcept;
    static std::int64_t SteadyNs() noexcept;

    // ---- 时间模拟（GM / 测试）。改完立刻刷新缓存，不用再 Update ----

    static void SetOffsetMs(std::int64_t offset_ms) noexcept;
    static void AddOffsetMs(std::int64_t delta_ms) noexcept;
    static std::int64_t offset_ms() noexcept { return State().offset_ms.load(std::memory_order_relaxed); }
    // 冻结在 fixed_ms（含 0 与负数）。冻结期间 Ms / LiveMs 都返回它。
    static void FreezeMs(std::int64_t fixed_ms) noexcept;
    static void Unfreeze() noexcept;
    static bool frozen() noexcept { return State().frozen_ms.load(std::memory_order_relaxed) != kUnset; }

    // ---- 业务时区 ----

    static std::int32_t TimezoneOffsetSec() noexcept;
    static void SetTimezoneOffsetSec(std::int32_t offset_sec) noexcept;
    // 重新采样本机时区（含当前是否夏令时）。受 TZ 环境变量影响。
    static void ResetTimezoneToLocal() noexcept;

private:
    static constexpr std::int64_t kUnset = std::numeric_limits<std::int64_t>::min();

    struct StateT
    {
        std::atomic<std::int64_t> cached_ms{kUnset};
        std::atomic<std::int64_t> offset_ms{0};
        std::atomic<std::int64_t> frozen_ms{kUnset};     // kUnset = 未冻结（0 是合法的冻结值）
        std::atomic<std::int64_t> tz_offset_sec{kUnset}; // kUnset = 还没取过本机时区
    };
    static StateT& State() noexcept
    {
        static constinit StateT s;
        return s;
    }
};

} // namespace breeze
