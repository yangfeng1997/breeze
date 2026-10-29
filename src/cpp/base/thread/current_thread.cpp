#include "current_thread.h"

#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstring>

namespace breeze::this_thread
{

namespace detail
{
__thread pid_t t_tid = 0;
} // namespace detail

namespace
{

// 内核限制线程名 15 个字符（加结尾 '\0' 共 16）；缓冲留大一点，pthread_getname_np 要求至少 16。
constexpr std::size_t kNameCap = 32;
constexpr std::size_t kMaxName = 15;

__thread char t_name[kNameCap]; // 空串 = 还没设置、也没取过

// 主线程的 tid 就是进程 pid；缓存下来省掉 getpid 的系统调用。0 = 还没取。
// 不在静态初始化里取：静态初始化的顺序不受控，别的翻译单元可能更早调到 IsMainThread。
std::atomic<pid_t> g_main_tid{0};

// 子进程里只剩调用 fork 的这个线程：它的 tid 变了，也成了主线程。
void AfterFork() noexcept
{
    detail::t_tid = 0;
    t_name[0] = '\0'; // 操作系统里的名字随 fork 保留，下次取用时重新读
    g_main_tid.store(0, std::memory_order_relaxed);
}

} // namespace

namespace detail
{

pid_t InitTid() noexcept
{
    // 首次取 tid 时登记 fork 回调：在那之前没有任何缓存，fork 也就不需要重置。magic static 保证只登记一次、线程安全。
    static const bool registered = (::pthread_atfork(nullptr, nullptr, &AfterFork), true);
    (void)registered;
    t_tid = static_cast<pid_t>(::syscall(SYS_gettid));
    return t_tid;
}

} // namespace detail

const char* Name() noexcept
{
    if (t_name[0] == '\0')
    {
        if (::pthread_getname_np(::pthread_self(), t_name, kNameCap) != 0 || t_name[0] == '\0')
            std::strcpy(t_name, "unknown");
    }
    return t_name;
}

void SetName(std::string_view name) noexcept
{
    if (name.empty())
        return;
    const std::size_t n = name.size() < kMaxName ? name.size() : kMaxName;
    std::memcpy(t_name, name.data(), n);
    t_name[n] = '\0';
    ::pthread_setname_np(::pthread_self(), t_name);
}

bool IsMainThread() noexcept
{
    pid_t main = g_main_tid.load(std::memory_order_relaxed);
    if (main == 0)
    {
        main = ::getpid();
        g_main_tid.store(main, std::memory_order_relaxed);
    }
    return Tid() == main;
}

} // namespace breeze::this_thread
