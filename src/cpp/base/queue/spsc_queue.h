// 单生产者单消费者的无界队列，包 moodycamel::ReaderWriterQueue；公开接口不出现它的类型。
//
// 契约：
// - 恰一个生产线程、一个消费线程；Debug 构建会断言违反。
// - TryDequeueBulk 返回 n < limit ⇒ 队列已空。
// - TryEnqueue 失败 = 没有预分配空间，不是容量上限；需要硬上限的调用方自带计数。
// - PeakApprox 是历史峰值的**近似下界**：首次、此后每 kPeakSampleEvery 次批量出队用 size_approx 采样一次，
//   其余调用用"本次取出的条数"更新；短于采样间隔的积压可能被低估。
//   不能每次都采样：ReaderWriterQueue::size_approx 要遍历全部块，随队列深度增长，消费者落后时越采样越慢、越积压
//   （对照实验见 docs/Actor与线程模型设计.md §9；queue_test 的 TestSpscPeak*）。
// - 队列不负责唤醒，也没有阻塞版本。

#pragma once

#include <readerwriterqueue.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "base/queue/internal/thread_role.h"

namespace breeze
{

template <typename T>
class SpscQueue
{
public:
    // initial_capacity 是首块预分配的元素数，TryEnqueue 只在它之内成功。
    explicit SpscQueue(std::size_t initial_capacity = 15)
        : q_(initial_capacity)
    {
    }

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    // 无界：空间不够就分配新块，不失败。
    void Enqueue(T&& v)
    {
        producer_.Check();
        (void)q_.enqueue(std::move(v));
    }
    void Enqueue(const T& v)
    {
        producer_.Check();
        (void)q_.enqueue(v);
    }

    // 不分配：没有预分配空间时返回 false，元素保持不变。
    bool TryEnqueue(T&& v)
    {
        producer_.Check();
        return q_.try_enqueue(std::move(v));
    }
    bool TryEnqueue(const T& v)
    {
        producer_.Check();
        return q_.try_enqueue(v);
    }

    bool TryDequeue(T& out)
    {
        consumer_.Check();
        T* p = q_.peek();
        if (p == nullptr)
            return false;
        out = std::move(*p);
        q_.pop();
        return true;
    }

    // 最多取 limit 个，依次写进 out（输出迭代器）；返回取到的个数。
    template <typename OutIt>
    std::size_t TryDequeueBulk(OutIt out, std::size_t limit)
    {
        consumer_.Check();
        SamplePeak();
        std::size_t n = 0;
        while (n < limit)
        {
            T* p = q_.peek();
            if (p == nullptr)
                break;
            *out++ = std::move(*p);
            q_.pop();
            ++n;
        }
        BumpPeak(n); // 零开销的下界：本次取出了这么多，队列至少有过这么深
        return n;
    }

    std::size_t SizeApprox() const { return q_.size_approx(); }

    // 采样间隔（调用次数，必须是 2 的幂）。一次采样的代价与队列深度成正比（块最大 512 个元素，逐块走）：
    // 8 条车道、积压无界（每条上百万）时吞吐占不采样的比例：间隔 64 约 31%、256 约 62%、1024 约 73%、4096 约 86%；
    // 每车道积压 ≤ 4096 时连 64 也与不采样持平。峰值只是监控用的近似下界，取 1024。
    static constexpr std::uint32_t kPeakSampleEvery = 1024;
    static_assert((kPeakSampleEvery & (kPeakSampleEvery - 1)) == 0, "kPeakSampleEvery 必须是 2 的幂（用掩码取模）");

    // 历史峰值的近似下界（见文件头契约）。生产者路径没有额外开销。
    std::size_t PeakApprox() const noexcept { return peak_.load(std::memory_order_relaxed); }

private:
    void SamplePeak() noexcept
    {
        if ((sample_tick_++ & (kPeakSampleEvery - 1)) != 0)
            return; // 首次调用 tick 为 0，会采样
        BumpPeak(q_.size_approx());
    }

    void BumpPeak(std::size_t v) noexcept
    {
        if (v > peak_.load(std::memory_order_relaxed))
            peak_.store(v, std::memory_order_relaxed); // 只有消费者写
    }

    moodycamel::ReaderWriterQueue<T> q_;
    queue_detail::ThreadRole producer_;
    queue_detail::ThreadRole consumer_;
    std::atomic<std::size_t> peak_{0};
    std::uint32_t sample_tick_ = 0; // 只有消费者读写，不需要原子
};

} // namespace breeze
