// 完成模型诊断快照；每 loop 单写、多线程只读，不能通过遍历在途请求取得统计。
#pragma once
#include <atomic>
#include <cstdint>

namespace breeze
{
struct IoStats
{
    std::uint64_t submissions = 0;
    std::uint64_t completions = 0;
    std::uint64_t receive_buffer_exhaustions = 0;
    std::uint64_t budget_exhaustions = 0;
    std::uint64_t cancellations = 0;
    std::uint64_t receive_buffer_bytes = 0;
    std::uint64_t pending_send_bytes = 0;
};
struct IoCounters
{
    std::atomic<std::uint64_t> submissions{0}, completions{0}, receive_buffer_exhaustions{0};
    std::atomic<std::uint64_t> budget_exhaustions{0}, cancellations{0}, receive_buffer_bytes{0}, pending_send_bytes{0};
    static void Add(std::atomic<std::uint64_t>& value, std::uint64_t n = 1) noexcept
    {
        value.store(value.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
    }
    IoStats Get() const noexcept
    {
        return {submissions.load(std::memory_order_relaxed),
                completions.load(std::memory_order_relaxed),
                receive_buffer_exhaustions.load(std::memory_order_relaxed),
                budget_exhaustions.load(std::memory_order_relaxed),
                cancellations.load(std::memory_order_relaxed),
                receive_buffer_bytes.load(std::memory_order_relaxed),
                pending_send_bytes.load(std::memory_order_relaxed)};
    }
};
} // namespace breeze
