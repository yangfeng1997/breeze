// 防御式断言的 Release 路径（BREEZE_ASSERT_FATAL=0）：失败后的控制流、expr 恰好求值一次、按调用点 2 的幂次限流记日志。
// Debug 路径（失败即 abort）在 assert_fatal_test.cpp，要 fork，另起一个进程。

#define BREEZE_ASSERT_FATAL 0
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "assert_macros.h"
#include "assert_test_util.h"

using breeze::testing::CaptureStderr;
using breeze::testing::Count;

namespace
{

int g_evals = 0;
bool Eval(bool v)
{
    ++g_evals;
    return v;
}

void RetVoid(bool ok, int& reached)
{
    ASSERT_RET_VOID(ok);
    reached = 1;
}

int RetVal(bool ok)
{
    ASSERT_RET_VAL(ok, -7);
    return 1;
}

void TestPassIsSilent()
{
    int reached = 0;
    const std::string out = CaptureStderr(
        [&]
        {
            ASSERT(true);
            RetVoid(true, reached);
            CHECK(RetVal(true) == 1);
            for (int i = 0; i < 3; ++i)
            {
                ASSERT_CONTINUE(i >= 0);
                ASSERT_BREAK(i < 100);
            }
        });
    CHECK(reached == 1);
    CHECK(out.empty());
}

void TestControlFlow()
{
    int reached = 0;
    const std::string out = CaptureStderr(
        [&]
        {
            RetVoid(false, reached);
            CHECK(reached == 0); // return 了，后面的语句没执行
            CHECK(RetVal(false) == -7);

            int after = 0;
            ASSERT(1 + 1 == 3); // 只记日志，继续往下走
            after = 1;
            CHECK(after == 1);

            // continue 作用在外层 for 上（不是宏内部的某层 do/while）
            int sum = 0;
            for (int i = 0; i < 10; ++i)
            {
                ASSERT_CONTINUE(i % 2 == 0);
                sum += i;
            }
            CHECK(sum == 0 + 2 + 4 + 6 + 8);

            int visited = 0;
            for (int i = 0; i < 10; ++i)
            {
                ASSERT_BREAK(i < 3);
                ++visited;
            }
            CHECK(visited == 3);
        });
    CHECK(Count(out, "ASSERT failed") > 0);
}

void TestEvaluatedOnce()
{
    g_evals = 0;
    const std::string out = CaptureStderr(
        [&]
        {
            ASSERT(Eval(true));
            ASSERT(Eval(false));
            int r = 0;
            RetVoid(Eval(false), r);
        });
    CHECK(g_evals == 3); // 通过与失败、两种模式下都恰好求值一次
}

// 同一个调用点反复失败：只在第 1、2、4、…、512 次记（1000 以内 10 条）。
void FailAtOneSite(int times)
{
    for (int i = 0; i < times; ++i)
    {
        volatile int value = 41;
        ASSERT(value == 42);
    }
}

void TestThrottlePerSite()
{
    const std::string out = CaptureStderr([] { FailAtOneSite(1000); });
    CHECK(Count(out, "ASSERT failed: (value == 42)") == 10);
    CHECK(out.find("第 1 次失败") != std::string::npos);
    CHECK(out.find("第 512 次失败") != std::string::npos);
    CHECK(out.find("第 3 次失败") == std::string::npos);
    CHECK(out.find("第 1000 次失败") == std::string::npos);
    CHECK(out.find("assert_test.cpp") != std::string::npos); // 记的是调用点位置
    CHECK(out.find("backtrace:") != std::string::npos);
    CHECK(out.find("#0 0x") != std::string::npos);
}

// 另一个调用点有自己的计数：第一次失败照常记。
void FailAtAnotherSite()
{
    volatile int other = 0;
    ASSERT(other == 1);
}

void TestSitesAreIndependent()
{
    const std::string out = CaptureStderr(
        []
        {
            FailAtAnotherSite();
            FailAtAnotherSite();
            FailAtAnotherSite();
        });
    CHECK(Count(out, "ASSERT failed: (other == 1)") == 2); // 第 1、2 次；第 3 次不记
}

// 并发用例自己的调用点：计数器是调用点私有、整个进程共享的，借用 FailAtOneSite 的话起点就不是 0 了。
void FailAtConcurrentSite(int times)
{
    for (int i = 0; i < times; ++i)
    {
        volatile int shared = 0;
        ASSERT(shared == 1);
    }
}

// 多线程撞同一个调用点：fetch_add 让每个 n 只出现一次，总数确定。
void TestThrottleConcurrent()
{
    constexpr int kThreads = 8;
    constexpr int kEach = 10000; // 总共 80000：2^0 … 2^16 共 17 个幂
    const std::string out = CaptureStderr(
        []
        {
            std::vector<std::thread> ts;
            for (int t = 0; t < kThreads; ++t)
                ts.emplace_back([] { FailAtConcurrentSite(kEach); });
            for (auto& t : ts)
                t.join();
        });
    CHECK(Count(out, "ASSERT failed: (shared == 1)") == 17);
}

} // namespace

int main()
{
    TestPassIsSilent();
    TestControlFlow();
    TestEvaluatedOnce();
    TestThrottlePerSite();
    TestSitesAreIndependent();
    TestThrottleConcurrent();
    std::puts("assert_test: ok");
    return 0;
}
