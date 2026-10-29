// 无测试框架依赖：失败即 abort。

#include "thread.h"

#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

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

std::string CurrentThreadName()
{
    char buf[32] = {};
    pthread_getname_np(pthread_self(), buf, sizeof(buf));
    return buf;
}

void TestName()
{
    std::string got;
    Thread t;
    CHECK(t.Start([&] { got = CurrentThreadName(); }, "net-test") == 0);
    t.Join();
    CHECK(got == "net-test");

    std::string longname;
    Thread t2;
    CHECK(t2.Start([&] { longname = CurrentThreadName(); }, "0123456789abcdefXYZ") == 0);
    t2.Join();
    CHECK(longname == "0123456789abcde"); // 内核限制 15 个字符，超出截断而不是失败
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
    CHECK(token.use_count() == 1); // 失败路径销毁了 fn，没有残留引用
    t.Join();                      // 失败的对象 Join 是空操作
}

} // namespace

int main()
{
    TestStartJoin();
    TestName();
    TestMoveOnlyCallable();
    TestStartFailureReturnsErrno();
    std::puts("thread_test: ok");
    return 0;
}
