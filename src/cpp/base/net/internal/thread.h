// pthread 封装。不用 std::thread / std::jthread：它们创建失败时抛 std::system_error，
// 而 net 库内不抛不 catch，创建失败要以 errno 返回（设计见 docs/网络库设计.md §4.11）。
//
// 谁创建线程，谁负责 Join；对象析构时仍未 Join 是编程错误。

#pragma once

#include <pthread.h>

#include <functional>
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

    // 返回 errno（0 = 成功，失败时线程没有创建、fn 已销毁）。name 最长 15 个字符，超出截断（内核限制）。
    // 对已启动且未 Join 的对象再次 Start 是编程错误。
    [[nodiscard]] int Start(std::move_only_function<void()> fn, std::string_view name = {});

    // 对没有启动、或已经 Join 过的对象是空操作。
    void Join();

    bool Joinable() const noexcept { return joinable_; }
    // 诊断用（比如读线程的 CPU 时间）。
    pthread_t native_handle() const noexcept { return tid_; }

private:
    pthread_t tid_{};
    bool joinable_ = false;
};

} // namespace breeze
