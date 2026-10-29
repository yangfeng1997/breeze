// 队列的线程角色断言：Debug 下记住第一个调用的线程，之后换线程即断言；Release 下是空对象。
// 只用于 SPSC/MPSC 里"必须恰一个线程"的角色，不用于 MPSC 的多生产者一侧。

#pragma once

#include <atomic>
#include <cassert>
#include <thread>

namespace breeze::queue_detail
{

class ThreadRole
{
public:
    void Check() noexcept
    {
#ifndef NDEBUG
        auto expected = std::thread::id{};
        if (!owner_.compare_exchange_strong(expected, std::this_thread::get_id(), std::memory_order_relaxed))
            assert(expected == std::this_thread::get_id() && "队列线程角色被违反：该角色只允许一个线程");
#endif
    }

private:
#ifndef NDEBUG
    std::atomic<std::thread::id> owner_{};
#endif
};

} // namespace breeze::queue_detail
