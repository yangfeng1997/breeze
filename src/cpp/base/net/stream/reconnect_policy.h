// StreamClient 的重连退避。设计见 docs/网络库设计.md §4.8。

#pragma once

#include <chrono>

namespace breeze
{

// 无限重试，要停由调用方 Stop()；没有"有限次数"这个选项。连接成功后退避重置回 initial_delay。
struct ReconnectPolicy
{
    std::chrono::milliseconds initial_delay{500};
    std::chrono::milliseconds max_delay{30000};
    double multiplier = 2.0; // 每失败一次，基准等待乘以它（≥ 1）
    // 抖动比例 [0, 1]：实际等待 = 基准 × (1 ± jitter 内的随机数)，错开大量对端同时重连。结果不超过 max_delay。
    double jitter = 0.2;

    // 字段是否合法：initial_delay > 0、max_delay ≥ initial_delay、multiplier ≥ 1、jitter 在 [0, 1]。
    constexpr bool Valid() const noexcept
    {
        return initial_delay.count() > 0 && max_delay >= initial_delay && multiplier >= 1.0 && jitter >= 0.0 &&
               jitter <= 1.0;
    }
};

} // namespace breeze
