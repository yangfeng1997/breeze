// pthread 封装。不用 std::thread / std::jthread：它们创建失败时抛 std::system_error，
// 而 net 库内不抛不 catch，创建失败要以 errno 返回（设计见 docs/网络库设计.md §4.11）。
//
// 谁创建线程，谁负责 Join；对象析构时仍未 Join 是编程错误（LOG_FATAL）：静默 detach 会让线程继续使用已经销毁的对象。
// 线程函数里抛出的异常不处理：没有 handler 时直接 std::terminate，默认的 terminate 处理器会打印异常类型与 what()，
// 且栈没有展开，core 里能看到抛出点；catch 之后再 abort 反而丢了现场。
//
// Start 返回时线程已经在运行、tid 与线程名都已就绪（Tid() 可用）。

#pragma once

#include <pthread.h>
#include <sys/types.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace breeze
{

class Thread
{
public:
    Thread() = default;
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;
    ~Thread();

    // 返回 errno（0 = 成功，失败时线程没有创建、fn 已销毁）。阻塞到新线程已经开始运行。
    // name 最长 15 个字符，超出截断（内核限制）；为空则自动命名 "Thread-N"（N 是进程内的创建序号），
    // 不然新线程会继承创建者的名字，top / gdb 里分不清谁是谁。
    // 对已启动且未 Join 的对象再次 Start 是编程错误。
    [[nodiscard]] int Start(std::move_only_function<void()> fn, std::string_view name = {});

    // 对没有启动、或已经 Join 过的对象是空操作。
    void Join();

    bool Joinable() const noexcept { return joinable_; }
    // 线程的内核 tid；Start 成功之后到 Join 之前有效，其余时候是 0。
    pid_t Tid() const noexcept { return tid_.load(std::memory_order_acquire); }
    // Start 时用的线程名（自动命名的是 "Thread-N"），没有截断。
    const std::string& name() const noexcept { return name_; }
    // 诊断用（比如读线程的 CPU 时间）。
    pthread_t native_handle() const noexcept { return handle_; }

    // 绑到指定 CPU 核心（Linux）。返回 errno：线程不在运行或核心号越界是 EINVAL。
    [[nodiscard]] int SetAffinity(int cpu) const noexcept;

    // 进程内已经成功创建的线程数。
    static std::uint32_t NumCreated() noexcept;

private:
    pthread_t handle_{};
    bool joinable_ = false;
    std::atomic<pid_t> tid_{0};
    std::string name_;
};

} // namespace breeze
