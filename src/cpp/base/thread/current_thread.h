// 当前线程的信息：缓存的 tid、线程名、是否主线程。
//
// - tid 首次取用时 syscall(SYS_gettid)，之后读线程局部缓存，快路径内联、零系统调用。
// - fork 安全：子进程里只剩调用 fork 的那个线程，它的 tid 缓存与"主线程"判断都会重置
//   （pthread_atfork 在首次取 tid 时登记；在此之前没有缓存，也就无需重置）。
// - 线程名存在线程局部的定长缓冲里，不是指针：线程退出后仍然安全，也不依赖谁来保管字符串。
// - 不提供 SleepUs / Yield 之类的包装：用 std::this_thread::sleep_for / yield（libstdc++ 的 sleep_for 会处理 EINTR）。

#pragma once

#include <sys/types.h>

#include <string_view>

namespace breeze::this_thread
{

namespace detail
{
// 直接暴露线程局部变量，是为了让 Tid() 的快路径能在调用方内联（日志等热路径）。用 GNU 的 __thread：
// 只有平凡类型能用，但访问不经过 thread_local 的初始化包装函数。
extern __thread pid_t t_tid;
[[gnu::cold]] pid_t InitTid() noexcept;
} // namespace detail

// 当前线程的内核 tid。
inline pid_t Tid() noexcept
{
    if (__builtin_expect(detail::t_tid == 0, 0))
        return detail::InitTid();
    return detail::t_tid;
}

// 线程名，最长 15 个字符。没设置过就取操作系统里的名字（主线程是可执行文件名）。返回的指针只在本线程内有效。
const char* Name() noexcept;

// 设置当前线程的名字（同时设给操作系统，top / gdb 里可见）。超过 15 个字符截断；空串是空操作。
void SetName(std::string_view name) noexcept;

// 是否进程的主线程（tid == pid）。不发系统调用；fork 之后调用 fork 的线程成为子进程的主线程。
bool IsMainThread() noexcept;

} // namespace breeze::this_thread
