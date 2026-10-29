// 自旋锁：只适合"临界区极短、几乎不会被抢占"的场景（几条指令的指针 / 计数更新）。
// 临界区里有分配、系统调用、日志、或者可能阻塞，请用 std::mutex。
//
// 与老实现相比：
//   - 先读后抢（TTAS）：等待时只读缓存行（relaxed load），不反复 test_and_set 把缓存行在核间抢来抢去；
//   - 指数退避 + CPU 暂停指令（x86 PAUSE、ARM YIELD）；
//   - 自旋超过一定次数就 sched_yield 让出 CPU：持锁线程被调度出去、或线程数多于核数时，纯忙等会烧满整个时间片，
//     老实现没有这一步；
//   - CPU 暂停指令是内联函数而不是 CPU_PAUSE 这种未加前缀的宏，不污染命名空间；去掉 C++20 已废弃的 ATOMIC_FLAG_INIT；
//   - 不提供自己的 Guard：直接用 std::lock_guard / std::scoped_lock / std::unique_lock（满足 BasicLockable /
//   Lockable）。
// 不可重入，不保证公平。

#pragma once

#include <sched.h>

#include <atomic>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace breeze
{

namespace detail
{
// 告诉 CPU 当前在自旋：降低功耗、让出超线程兄弟核的执行资源、避免退出自旋时的内存序流水线清空。
inline void CpuRelax() noexcept
{
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#endif
}
} // namespace detail

class SpinLock
{
public:
    SpinLock() = default;
    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;

    void lock() noexcept
    {
        while (flag_.test_and_set(std::memory_order_acquire))
            WaitUntilFree();
    }

    bool try_lock() noexcept
    {
        return !flag_.test(std::memory_order_relaxed) && !flag_.test_and_set(std::memory_order_acquire);
    }

    void unlock() noexcept { flag_.clear(std::memory_order_release); }

private:
    // 退避到抢锁的次数上限、之后改为让出 CPU 的阈值。数值是经验值：64 次 PAUSE 约一两百纳秒到一两微秒，
    // 比这更久说明持锁者多半被抢占了，继续忙等没有意义。
    static constexpr int kMaxBackoff = 64;
    static constexpr int kSpinsBeforeYield = 12;

    void WaitUntilFree() noexcept
    {
        int backoff = 1;
        for (int spins = 0; flag_.test(std::memory_order_relaxed); ++spins)
        {
            if (spins >= kSpinsBeforeYield)
            {
                ::sched_yield();
                continue;
            }
            for (int i = 0; i < backoff; ++i)
                detail::CpuRelax();
            if (backoff < kMaxBackoff)
                backoff <<= 1;
        }
    }

    std::atomic_flag flag_;
};

} // namespace breeze
