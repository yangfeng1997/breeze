// base/assert 测试共用的小工具（只给 *_test.cpp 用，不进库）：CHECK、捕获 stderr、"必须 abort 并留下这些文字"的断言。

#pragma once

#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>

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

inline std::string ReadAll(int fd)
{
    std::string out;
    char buf[4096];
    for (;;)
    {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        out.append(buf, static_cast<std::size_t>(n));
    }
    return out;
}

inline std::size_t Count(const std::string& hay, const std::string& needle)
{
    std::size_t n = 0;
    for (std::size_t pos = hay.find(needle); pos != std::string::npos; pos = hay.find(needle, pos + needle.size()))
        ++n;
    return n;
}

// 在当前进程里跑 fn，期间 fd 2 指向临时文件，返回这段时间写到 stderr 的全部内容。
// 日志兜底实例写的是 fd 2，所以能抓到。fn 里不要起不受控的线程到 fn 返回后还在写。
inline std::string CaptureStderr(const std::function<void()>& fn)
{
    std::fflush(stderr);
    char path[] = "/tmp/breeze_assert_test_XXXXXX";
    const int tmp = ::mkstemp(path);
    CHECK(tmp >= 0);
    const int saved = ::dup(STDERR_FILENO);
    ::dup2(tmp, STDERR_FILENO);
    fn();
    std::fflush(stderr);
    ::dup2(saved, STDERR_FILENO);
    ::close(saved);
    ::lseek(tmp, 0, SEEK_SET);
    std::string out = ReadAll(tmp);
    ::close(tmp);
    ::unlink(path);
    return out;
}

// fork 出子进程跑 fn，期望它被 SIGABRT 杀死；stderr 内容经 output 带回。fn 正常返回或别的死法都算失败。
// fork 要求本进程此刻没有别的线程在跑。
inline bool ExpectAbort(const std::function<void()>& fn, std::string* output)
{
    std::fflush(nullptr);
    int fds[2];
    if (::pipe(fds) != 0)
        return false;
    const pid_t pid = ::fork();
    if (pid < 0)
        return false;
    if (pid == 0)
    {
        ::close(fds[0]);
        ::dup2(fds[1], STDERR_FILENO);
        ::close(fds[1]);
        ::signal(SIGABRT, SIG_DFL); // 默认的 core dump 会让子进程退出变慢
        fn();
        ::_exit(0);
    }
    ::close(fds[1]);
    std::string out = ReadAll(fds[0]);
    ::close(fds[0]);
    int status = 0;
    if (::waitpid(pid, &status, 0) != pid)
        return false;
    if (output != nullptr)
        *output = std::move(out);
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

} // namespace breeze::testing
