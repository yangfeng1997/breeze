// base/thread 测试共用的小工具（只给 *_test.cpp 用，不进库）：CHECK、带超时的等待、"必须 abort"的断言。
// 无测试框架依赖：失败即 abort，并发用例带超时，卡死也会 abort 而不是挂住 ctest。

#pragma once

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

namespace breeze::testing
{

using namespace std::chrono_literals;

constexpr auto kWait = 10s;

template <typename Pred>
bool WaitUntil(Pred pred, std::chrono::milliseconds timeout = kWait)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred())
    {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(200us);
    }
    return true;
}

// fork 出子进程跑 fn，期望它被 SIGABRT 杀死（LOG_FATAL 的结果）；fn 正常返回、或以别的方式退出都算失败。
// 子进程的 stderr 丢弃（LOG_FATAL 会打日志）。fork 要求调用时本进程没有别的线程在跑：用例开头先把线程都 Join 掉。
inline bool ExpectAbort(const std::function<void()>& fn)
{
    std::fflush(nullptr);
    const pid_t pid = ::fork();
    if (pid < 0)
        return false;
    if (pid == 0)
    {
        const int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0)
            ::dup2(devnull, STDERR_FILENO);
        // 默认的 core dump 会让子进程退出变慢，也没有意义
        ::signal(SIGABRT, SIG_DFL);
        fn();
        ::_exit(0);
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) != pid)
        return false;
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

} // namespace breeze::testing
