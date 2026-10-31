// 单生产者单消费者的无界队列，包 moodycamel::ReaderWriterQueue；公开接口不出现它的类型。
//
// 契约：
// - 恰一个生产线程、一个消费线程；Debug 构建会断言违反。
// - TryDequeueBulk 返回 n < limit ⇒ 队列已空。
// - TryEnqueue 失败 = 没有预分配空间，不是容量上限；需要硬上限的调用方自带计数。
// - 队列不负责唤醒，也没有阻塞版本。

#pragma once

#include <readerwriterqueue.h>

#include <atomic>
#include <cstddef>
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
        return n;
    }

    std::size_t SizeApprox() const { return q_.size_approx(); }

    // 历史峰值：消费者在每次批量出队前采样 SizeApprox，生产者路径没有额外开销。
    std::size_t PeakApprox() const noexcept { return peak_.load(std::memory_order_relaxed); }

private:
    void SamplePeak() noexcept
    {
        const std::size_t s = q_.size_approx();
        if (s > peak_.load(std::memory_order_relaxed))
            peak_.store(s, std::memory_order_relaxed); // 只有消费者写
    }

    moodycamel::ReaderWriterQueue<T> q_;
    queue_detail::ThreadRole producer_;
    queue_detail::ThreadRole consumer_;
    std::atomic<std::size_t> peak_{0};
};

} // namespace breeze
