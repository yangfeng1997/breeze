// Singleton / ThreadLocalSingleton：全进程只构造一次（并发首次访问）、线程局部各一份、退出时析构。
// 无测试框架依赖：失败即 abort。

#include "singleton.h"

#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <thread>
#include <vector>

using namespace breeze;

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

namespace
{

std::atomic<int> g_global_ctor{0};
int g_exit_fd = -1; // 子进程里：析构时往这个管道写一个字节

class Global : public Singleton<Global>
{
    friend class Singleton<Global>;
    Global()
    {
        // 让并发的首次访问确实重叠：构造得慢一点，没有互斥的话别的线程会同时进来
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        g_global_ctor.fetch_add(1);
    }
    ~Global()
    {
        if (g_exit_fd >= 0)
            (void)!::write(g_exit_fd, "x", 1);
    }

public:
    int value = 0;
};

void TestConcurrentFirstAccess()
{
    constexpr int kThreads = 16;
    std::atomic<bool> go{false};
    std::vector<Global*> seen(kThreads, nullptr);
    std::vector<std::thread> ts;
    for (int i = 0; i < kThreads; ++i)
    {
        ts.emplace_back(
            [&, i]
            {
                while (!go.load())
                    std::this_thread::yield();
                seen[static_cast<std::size_t>(i)] = &Global::Instance();
            });
    }
    go = true;
    for (auto& t : ts)
        t.join();
    CHECK(g_global_ctor.load() == 1);
    for (auto* p : seen)
        CHECK(p == &Global::Instance());
}

void TestSharedState()
{
    Global::Instance().value = 5;
    int seen = 0;
    std::thread([&] { seen = Global::Instance().value; }).join();
    CHECK(seen == 5);
}

// 正常退出时 static 局部对象被析构：在子进程里 exit(0)，由析构往管道写字节。
void TestDestroyedAtExit()
{
    int fds[2];
    CHECK(::pipe(fds) == 0);
    std::fflush(nullptr);
    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0)
    {
        ::close(fds[0]);
        g_exit_fd = fds[1];
        (void)Global::Instance(); // 子进程里首次构造（fork 之前父进程已经构造过，这里是继承来的那份）
        std::exit(0);
    }
    ::close(fds[1]);
    char c = 0;
    const ssize_t n = ::read(fds[0], &c, 1);
    ::close(fds[0]);
    int status = 0;
    CHECK(::waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(n == 1 && c == 'x');
}

std::atomic<int> g_tls_ctor{0};
std::atomic<int> g_tls_dtor{0};

class PerThread : public ThreadLocalSingleton<PerThread>
{
    friend class ThreadLocalSingleton<PerThread>;
    PerThread() { g_tls_ctor.fetch_add(1); }
    ~PerThread() { g_tls_dtor.fetch_add(1); }

public:
    int value = 0;
};

void TestThreadLocalDistinct()
{
    constexpr int kThreads = 8;
    std::vector<PerThread*> addrs(kThreads, nullptr);
    std::vector<int> read_back(kThreads, -1);
    std::atomic<int> arrived{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < kThreads; ++i)
    {
        ts.emplace_back(
            [&, i]
            {
                PerThread& a = PerThread::Instance();
                a.value = i * 10;
                addrs[static_cast<std::size_t>(i)] = &a;
                // 等所有线程都到齐再继续：先结束的线程会把 TLS 让给后来的线程，地址就可能重复
                arrived.fetch_add(1);
                while (arrived.load() < kThreads)
                    std::this_thread::yield();
                CHECK(&PerThread::Instance() == &a); // 同一线程每次拿到同一个
                read_back[static_cast<std::size_t>(i)] = PerThread::Instance().value;
            });
    }
    for (auto& t : ts)
        t.join();

    std::set<PerThread*> uniq(addrs.begin(), addrs.end());
    CHECK(uniq.size() == kThreads); // 线程同时存活，地址互不相同
    for (int i = 0; i < kThreads; ++i)
        CHECK(read_back[static_cast<std::size_t>(i)] == i * 10);
    CHECK(g_tls_ctor.load() == kThreads);
    CHECK(g_tls_dtor.load() == kThreads); // 线程退出时析构，join 返回时已经完成
}

} // namespace

int main()
{
    TestConcurrentFirstAccess();
    TestSharedState();
    TestDestroyedAtExit();
    TestThreadLocalDistinct();
    std::puts("singleton_test: ok");
    return 0;
}
