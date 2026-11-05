// 多生产者单消费者的无界队列，包 moodycamel::ConcurrentQueue；公开接口不出现它的类型。
//
// 契约：
// - 多个生产线程、恰一个消费线程；消费侧在 Debug 构建下断言违反。
// - 只保证每个生产者内部有序，跨生产者没有全局顺序。
// - TryDequeueBulk 返回 n < limit ⇒ 队列已空。
// - TryEnqueue 失败 = 没有预分配空间，不是容量上限；需要硬上限的调用方自带计数。
// - 固定的长寿生产线程可以持有 ProducerToken，比隐式生产者快；token 必须先于队列销毁。
// - PeakApprox 是历史峰值的**近似下界**：首次、此后每 kPeakSampleEvery 次批量出队用 size_approx 采样一次，
//   其余调用用"本次取出的条数"更新；短于采样间隔的积压可能被低估。
//   不能每次都采样：ConcurrentQueue::size_approx 要遍历所有生产者并读各自的 tailIndex，
//   生产者多时明显拖慢批量出队（31 个生产者约 -37%，对照实验见 docs/Actor与线程模型设计.md §9；queue_test 的 TestMpscPeak*）。
// - 队列不负责唤醒，也没有阻塞版本。

#pragma once

#include <moodycamel/concurrentqueue.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
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
        const std::size_t n = q_.try_dequeue_bulk(out, limit);
        BumpPeak(n); // 零开销的下界：本次取出了这么多，队列至少有过这么深
        return n;
    }

    std::size_t SizeApprox() const { return q_.size_approx(); }

    // 采样间隔（调用次数，必须是 2 的幂）。1024 次调用一次：size_approx 要遍历所有生产者，不能进热路径；
    // 峰值只是监控用的近似下界（SpscQueue 上的对照实验见它的同名常量）。
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

    moodycamel::ConcurrentQueue<T> q_;
    queue_detail::ThreadRole consumer_;
    std::atomic<std::size_t> peak_{0};
    std::uint32_t sample_tick_ = 0; // 只有消费者读写，不需要原子
};

} // namespace breeze
