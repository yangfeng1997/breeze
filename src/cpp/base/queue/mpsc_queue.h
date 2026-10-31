// 多生产者单消费者的无界队列，包 moodycamel::ConcurrentQueue；公开接口不出现它的类型。
//
// 契约：
// - 多个生产线程、恰一个消费线程；消费侧在 Debug 构建下断言违反。
// - 只保证每个生产者内部有序，跨生产者没有全局顺序。
// - TryDequeueBulk 返回 n < limit ⇒ 队列已空。
// - TryEnqueue 失败 = 没有预分配空间，不是容量上限；需要硬上限的调用方自带计数。
// - 固定的长寿生产线程可以持有 ProducerToken，比隐式生产者快；token 必须先于队列销毁。
// - 队列不负责唤醒，也没有阻塞版本。

#pragma once

#include <moodycamel/concurrentqueue.h>

#include <atomic>
#include <cstddef>
#include <utility>

#include "base/queue/internal/thread_role.h"

namespace breeze
{

template <typename T>
class MpscQueue
{
public:
    // 一个生产线程一个 token，不可复制；构造它的 MpscQueue 必须比它活得久。
    class ProducerToken
    {
    public:
        ProducerToken(ProducerToken&&) noexcept = default;
        ProducerToken& operator=(ProducerToken&&) = delete;

    private:
        friend class MpscQueue;
        explicit ProducerToken(moodycamel::ConcurrentQueue<T>& q)
            : tok_(q)
        {
        }
        moodycamel::ProducerToken tok_;
    };

    MpscQueue() = default;
    // initial_capacity 是预分配的元素数，TryEnqueue 只在它之内成功。
    explicit MpscQueue(std::size_t initial_capacity)
        : q_(initial_capacity)
    {
    }

    MpscQueue(const MpscQueue&) = delete;
    MpscQueue& operator=(const MpscQueue&) = delete;

    ProducerToken MakeProducerToken() { return ProducerToken(q_); }

    // 无界：空间不够就分配新块，不失败。
    void Enqueue(T&& v) { (void)q_.enqueue(std::move(v)); }
    void Enqueue(const T& v) { (void)q_.enqueue(v); }
    void Enqueue(ProducerToken& t, T&& v) { (void)q_.enqueue(t.tok_, std::move(v)); }
    void Enqueue(ProducerToken& t, const T& v) { (void)q_.enqueue(t.tok_, v); }

    // 不分配：没有预分配空间时返回 false。
    bool TryEnqueue(T&& v) { return q_.try_enqueue(std::move(v)); }
    bool TryEnqueue(const T& v) { return q_.try_enqueue(v); }
    bool TryEnqueue(ProducerToken& t, T&& v) { return q_.try_enqueue(t.tok_, std::move(v)); }
    bool TryEnqueue(ProducerToken& t, const T& v) { return q_.try_enqueue(t.tok_, v); }

    bool TryDequeue(T& out)
    {
        consumer_.Check();
        return q_.try_dequeue(out);
    }

    // 最多取 limit 个，依次写进 out（输出迭代器）；返回取到的个数。
    template <typename OutIt>
    std::size_t TryDequeueBulk(OutIt out, std::size_t limit)
    {
        consumer_.Check();
        SamplePeak();
        return q_.try_dequeue_bulk(out, limit);
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

    moodycamel::ConcurrentQueue<T> q_;
    queue_detail::ThreadRole consumer_;
    std::atomic<std::size_t> peak_{0};
};

} // namespace breeze
