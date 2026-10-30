// 周期性的开放窗口：从 start 起每隔 interval 开一次、每次持续 duration，
// 回答"现在开不开、这一期何时结束、下一期何时开"。周活动、每日限时玩法、一次性活动都是它的特例。
// 纯函数：没有状态、不读时钟，now 由调用方传入；时间戳单位秒（Unix 时间，UTC），与 TimeUtil 一致。
//
//   // 每周六 20:00（东八区）开 2 小时，从 2026-11-07 起
//   const auto start = *TimeUtil::Parse("2026-11-07 20:00:00", 8 * 3600);
//   const PeriodicWindow w{start, 2 * 3600, 7 * 86400};
//   if (w.Contains(Now::Sec())) ...      // 用 Now::Sec()：GM 的时间偏移 / 冻结才会生效，别用 time(nullptr)
//
// 第 k 期（k >= 0）是半开区间 [start + k*interval, start + k*interval + duration)：
// 起点那一秒是开的，终点那一秒已经关了。
//
// 取舍：
//   - interval == 0：只开一次（只有第 0 期）。
//   - duration == interval：首尾相接，终点那一秒就是下一期的起点，所以一直开着。duration > interval（两期重叠）不接受。
//   - 永久开放写 PeriodicWindow{start, kForever, 0}。**duration == 0 是空窗口，不是"无限"**：老代码里 0 被换成"十年"，
//     这里不沿用——配置漏填会变成"永远不开"，而不是"永远开着"。
//   - Valid() 为 false 的窗口（duration <= 0、interval < 0、duration > interval > 0）所有查询都当作"永远不开"，
//     不断言、不抛异常；配置加载时调一次 Valid() 把它拒掉。
//
// 前提：start、duration、interval、now 的绝对值都远小于 2^60（约 3.6e10 年），内部的加、乘不会溢出。
// 别拿 INT64_MAX 当"永不过期"。

#pragma once

#include <cstdint>
#include <optional>

namespace breeze
{

struct PeriodicWindow
{
    // 约 91 亿年，够当"永不关闭"，又留够余量让 start + duration 不溢出。
    static constexpr std::int64_t kForever = std::int64_t{1} << 58;

    std::int64_t start = 0;    // 第 0 期的起点
    std::int64_t duration = 0; // 每一期的长度（秒）
    std::int64_t interval = 0; // 相邻两期起点的间隔（秒）；0 = 不重复

    // interval < 0 不用单独判：duration > 0 时 duration <= interval 必然不成立。
    constexpr bool Valid() const noexcept { return duration > 0 && (interval == 0 || duration <= interval); }

    // now 落在第几期（从 0 起）；不在任何一期里返回 nullopt。
    constexpr std::optional<std::int64_t> Index(std::int64_t now) const noexcept
    {
        if (!Valid() || now < start)
            return std::nullopt;
        const std::int64_t rel = now - start;
        if (interval == 0)
            return rel < duration ? std::optional<std::int64_t>{0} : std::nullopt;
        return rel % interval < duration ? std::optional<std::int64_t>{rel / interval} : std::nullopt;
    }

    constexpr bool Contains(std::int64_t now) const noexcept { return Index(now).has_value(); }

    // now 所在这一期的起点 / 终点（不含）；不在窗口里返回 nullopt。
    constexpr std::optional<std::int64_t> CurrentBegin(std::int64_t now) const noexcept
    {
        const auto k = Index(now);
        return k ? std::optional<std::int64_t>{start + *k * interval} : std::nullopt;
    }
    constexpr std::optional<std::int64_t> CurrentEnd(std::int64_t now) const noexcept
    {
        const auto b = CurrentBegin(now);
        return b ? std::optional<std::int64_t>{*b + duration} : std::nullopt;
    }

    // 起点**严格晚于** now 的最早一期的起点：now 恰好是某期的起点时返回的是下一期。
    // 没有下一期（一次性窗口已经开始过、或窗口无效）返回 nullopt。
    constexpr std::optional<std::int64_t> NextBegin(std::int64_t now) const noexcept
    {
        if (!Valid())
            return std::nullopt;
        if (now < start)
            return start;
        if (interval == 0)
            return std::nullopt;
        return start + ((now - start) / interval + 1) * interval;
    }
};

} // namespace breeze
