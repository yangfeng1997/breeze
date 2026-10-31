// Thread：启停、tid 握手、线程名（含自动命名与截断）、绑核、创建失败返回 errno、误用即 FATAL。

#include "base/thread/thread.h"

#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "base/thread/current_thread.h"
#include "base/thread/thread_test_util.h"

using namespace breeze;
using namespace breeze::testing;

namespace
{

void TestStartJoin()
{
    std::atomic<int> ran{0};
    Thread t;
    CHECK(!t.Joinable());
    t.Join(); // 没启动过：空操作
    CHECK(t.Start([&] { ran.store(1); }) == 0);
    CHECK(t.Joinable());
    t.Join();
    CHECK(!t.Joinable());
    CHECK(ran.load() == 1); // Join 之后线程里的写入可见
    t.Join();               // 重复 Join：空操作
}

std::string OsThreadName()
{
    char buf[32] = {};
    pthread_getname_np(pthread_self(), buf, sizeof(buf));
    return buf;
}

void TestName()
{
    std::string got;
    Thread t;
    CHECK(t.Start([&] { got = OsThreadName(); }, "net-test") == 0);
    CHECK(t.name() == "net-test");
    t.Join();
    CHECK(got == "net-test");

    std::string longname;
    Thread t2;
    CHECK(t2.Start([&] { longname = OsThreadName(); }, "0123456789abcdefXYZ") == 0);
    t2.Join();
    CHECK(longname ==
          "0123456789abcde"); // 内核限制 15 个字符，超出截断而不是失败（pthread_setname_np 会报 ERANGE 且一个字也不设）
    CHECK(t2.name() == "0123456789abcdefXYZ"); // name() 保留完整的
}

// 没给名字：自动命名 "Thread-N"，而不是继承创建者的名字（top / gdb 里分不清谁是谁）。
void TestAutoName()
{
    std::string a;
    std::string b;
    Thread t1;
    Thread t2;
    CHECK(t1.Start([&] { a = OsThreadName(); }) == 0);
    CHECK(t2.Start([&] { b = OsThreadName(); }) == 0);
    t1.Join();
    t2.Join();
    CHECK(a.rfind("Thread-", 0) == 0 && b.rfind("Thread-", 0) == 0);
    CHECK(a != b);
    CHECK(t1.name() == a && t2.name() == b);
}

// Start 返回时 tid 已就绪，且与线程里看到的一致；Join 之后清零。
void TestTidHandshake()
{
    pid_t inside = 0;
    std::atomic<bool> go{false};
    Thread t;
    CHECK(t.Tid() == 0);
    CHECK(t.Start(
              [&]
              {
                  inside = this_thread::Tid();
                  while (!go.load())
                      std::this_thread::yield();
              }) == 0);
    const pid_t tid = t.Tid(); // 线程还卡在里面，没有任何别的同步：握手保证此刻 tid 已经可用
    CHECK(tid > 0);
    CHECK(tid != ::getpid());
    go = true;
    t.Join();
    CHECK(inside == tid);
    CHECK(t.Tid() == 0);
}

// 反复 Start / Join：握手不丢通知、不死等（等 tid 的 futex 路径）。
void TestStartJoinManyTimes()
{
    for (int i = 0; i < 2000; ++i)
    {
        std::atomic<int> v{0};
        Thread t;
        CHECK(t.Start([&] { v.store(i + 1); }) == 0);
        CHECK(t.Tid() > 0);
        t.Join();
        CHECK(v.load() == i + 1);
    }
}

void TestMoveOnlyCallable()
{
    int result = 0;
    auto p = std::make_unique<int>(41);
    Thread t;
    CHECK(t.Start([&result, p = std::move(p)] { result = *p + 1; }) == 0);
    t.Join();
    CHECK(result == 42);
}

// 绑核：挑一个本进程允许的核心（容器 / cgroup 里不一定有 0 号核）。
void TestAffinity()
{
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    CHECK(::sched_getaffinity(0, sizeof(allowed), &allowed) == 0);
    int cpu = -1;
    for (int i = 0; i < CPU_SETSIZE; ++i)
    {
        if (CPU_ISSET(i, &allowed))
        {
            cpu = i;
            break;
        }
    }
    CHECK(cpu >= 0);

    Thread t;
    CHECK(t.SetAffinity(cpu) == EINVAL); // 还没启动
    std::atomic<int> on{-1};
    std::atomic<bool> bound{false};
    CHECK(t.Start(
              [&]
              {
                  while (!bound.load())
                      std::this_thread::yield();
                  on = ::sched_getcpu();
              }) == 0);
    CHECK(t.SetAffinity(cpu) == 0);
    CHECK(t.SetAffinity(-1) == EINVAL);
    CHECK(t.SetAffinity(CPU_SETSIZE) == EINVAL);
    bound = true;
    t.Join();
    CHECK(on.load() == cpu);
    CHECK(t.SetAffinity(cpu) == EINVAL); // 已经 Join
}

void TestNumCreated()
{
    const auto before = Thread::NumCreated();
    Thread t;
    CHECK(t.Start([] {}) == 0);
    t.Join();
    CHECK(Thread::NumCreated() == before + 1);
}

// 创建失败返回 errno，且传进去的可调用对象已被销毁（不泄漏）。
// 用 RLIMIT_NPROC 制造；root 不受它限制，设置失败也直接跳过。
void TestStartFailureReturnsErrno()
{
    if (::geteuid() == 0)
    {
        std::puts("thread_test: 跳过 NPROC 失败用例（root 不受 RLIMIT_NPROC 限制）");
        return;
    }
    rlimit old{};
    if (::getrlimit(RLIMIT_NPROC, &old) != 0)
        return;
    rlimit low = old;
    low.rlim_cur = 1;
    if (::setrlimit(RLIMIT_NPROC, &low) != 0)
        return;

    const auto token = std::make_shared<int>(0);
    Thread t;
    const int rc = t.Start([token] {});
    ::setrlimit(RLIMIT_NPROC, &old); // 先还原，免得影响后面的用例
    CHECK(rc == EAGAIN);
    CHECK(!t.Joinable());
    CHECK(t.Tid() == 0);
    CHECK(token.use_count() == 1); // 失败路径销毁了 fn，没有残留引用
    t.Join();                      // 失败的对象 Join 是空操作
}

// 误用：重复 Start、析构时还没 Join——都是编程错误，直接 FATAL（静默 detach 会让线程继续用已销毁的对象）。
// 放在最后：fork 要求此刻没有别的线程。
void TestMisuseIsFatal()
{
    CHECK(ExpectAbort(
        []
        {
            std::atomic<bool> stop{false};
            Thread t;
            (void)t.Start(
                [&]
                {
                    while (!stop.load())
                        std::this_thread::yield();
                });
            (void)t.Start([] {}); // 重复 Start
        }));
    CHECK(ExpectAbort(
        []
        {
            std::atomic<bool> stop{false};
            {
                Thread t;
                (void)t.Start(
                    [&]
                    {
                        while (!stop.load())
                            std::this_thread::yield();
                    });
            } // 析构时仍未 Join
        }));
}

} // namespace

int main()
{
    TestStartJoin();
    TestName();
    TestAutoName();
    TestTidHandshake();
    TestStartJoinManyTimes();
    TestMoveOnlyCallable();
    TestAffinity();
    TestNumCreated();
    TestStartFailureReturnsErrno();
    TestMisuseIsFatal();
    std::puts("thread_test: ok");
    return 0;
}
