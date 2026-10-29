// 等待组：等一组并发任务全部完成。语义对标 Go 的 sync.WaitGroup。
//
//   WaitGroup wg;
//   wg.Add(n);
//   for (...) pool.Post([&] { Work(); wg.Done(); });
//   wg.Wait();
//
// 与 std::latch 的区别：计数可以在运行中再增加（Add），并且有带超时的等待。计数固定、只等一次时直接用 std::latch。
//
// 计数降到负数是编程错误（Done 多于 Add），LOG_FATAL——不抛异常。老实现里用无符号数 count_ - 1 会回绕成 4294967295，
// 之后 Wait 永远不返回。
// Add 的正增量如果发生在计数为 0 时，必须先于 Wait（同 Go 的约定）。
// 线程安全；Wait 返回之后立刻销毁对象是安全的（notify 在持锁时发出）。

#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace breeze
{

class WaitGroup
{
public:
    WaitGroup() = default;
    WaitGroup(const WaitGroup&) = delete;
    WaitGroup& operator=(const WaitGroup&) = delete;

    void Add(int delta = 1)
    {
        const std::lock_guard lk(mu_);
        count_ += delta;
        if (count_ < 0)
            FailNegative(count_, delta);
        if (count_ == 0)
            cv_.notify_all();
    }

    void Done() { Add(-1); }

    void Wait()
    {
        std::unique_lock lk(mu_);
        cv_.wait(lk, [this] { return count_ == 0; });
    }

    // 计数归零返回 true，超时返回 false。
    template <typename Rep, typename Period>
    bool WaitFor(const std::chrono::duration<Rep, Period>& timeout)
    {
        std::unique_lock lk(mu_);
        return cv_.wait_for(lk, timeout, [this] { return count_ == 0; });
    }

    template <typename Clock, typename Duration>
    bool WaitUntil(const std::chrono::time_point<Clock, Duration>& deadline)
    {
        std::unique_lock lk(mu_);
        return cv_.wait_until(lk, deadline, [this] { return count_ == 0; });
    }

    // 当前计数，只用于调试 / 监控。
    int Count() const
    {
        const std::lock_guard lk(mu_);
        return count_;
    }

private:
    [[noreturn]] static void FailNegative(int count, int delta);

    mutable std::mutex mu_;
    std::condition_variable cv_;
    int count_ = 0;
};

} // namespace breeze
