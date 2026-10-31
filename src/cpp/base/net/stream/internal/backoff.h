// 重连退避序列：基准按 multiplier 指数增长、封顶 max_delay，再乘一个 ±jitter 的随机因子。
// 只在所属 loop 线程使用。随机数是自带的 xorshift：不引入 <random>（random_device 失败会抛）。

#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

#include "base/net/stream/reconnect_policy.h"

namespace breeze
{

class Backoff
{
public:
    Backoff(const ReconnectPolicy& policy, std::uint64_t seed) noexcept
        : policy_(policy)
        , rng_(seed | 1) // xorshift 的状态不能为 0
        , base_ms_(static_cast<double>(policy.initial_delay.count()))
    {
    }

    // 本次要等多久，并把基准推进一步。
    std::chrono::milliseconds Next() noexcept
    {
        const double max_ms = static_cast<double>(policy_.max_delay.count());
        const double base = std::min(base_ms_, max_ms);
        base_ms_ = std::min(base_ms_ * policy_.multiplier, max_ms);
        const double factor = 1.0 + policy_.jitter * (2.0 * NextUnit() - 1.0);
        const auto ms = static_cast<std::int64_t>(std::llround(std::min(base * factor, max_ms)));
        return std::chrono::milliseconds(std::max<std::int64_t>(ms, 1));
    }

    // 连接成功（或重新 Start）后调用：下一次从 initial_delay 重新开始。
    void Reset() noexcept { base_ms_ = static_cast<double>(policy_.initial_delay.count()); }

private:
    // [0, 1)
    double NextUnit() noexcept
    {
        rng_ ^= rng_ >> 12;
        rng_ ^= rng_ << 25;
        rng_ ^= rng_ >> 27;
        const std::uint64_t r = rng_ * 0x2545F4914F6CDD1DULL;
        return static_cast<double>(r >> 11) * (1.0 / 9007199254740992.0); // 2^53
    }

    ReconnectPolicy policy_;
    std::uint64_t rng_;
    double base_ms_;
};

} // namespace breeze
