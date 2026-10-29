// WaitGroup：基本等待、动态 Add、复用、带超时的等待、多个等待者、等完立刻销毁，以及 Done 多于 Add 即 FATAL。

#include "wait_group.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <vector>

#include "thread.h"
#include "thread_test_util.h"

using namespace breeze;
using namespace breeze::testing;

namespace
{

void TestBasic()
{
    WaitGroup wg;
    wg.Wait(); // 计数为 0：立即返回
    CHECK(wg.Count() == 0);

    constexpr int kN = 8;
    std::atomic<int> done{0};
    std::vector<Thread> ts(kN);
    wg.Add(kN);
    CHECK(wg.Count() == kN);
    for (auto& t : ts)
    {
        CHECK(t.Start(
                  [&]
                  {
                      done.fetch_add(1);
                      wg.Done();
                  }) == 0);
    }
    wg.Wait();
    CHECK(done.load() == kN); // Wait 返回时所有任务的写入可见
    CHECK(wg.Count() == 0);
    for (auto& t : ts)
        t.Join();
}

// 计数归零之后可以再用。
void TestReuse()
{
    WaitGroup wg;
    for (int round = 0; round < 5; ++round)
    {
        std::atomic<int> n{0};
        wg.Add(3);
        std::vector<Thread> ts(3);
        for (auto& t : ts)
            CHECK(t.Start(
                      [&]
                      {
                          n.fetch_add(1);
                          wg.Done();
                      }) == 0);
        wg.Wait();
        CHECK(n.load() == 3);
        for (auto& t : ts)
            t.Join();
    }
}

// 任务里再 Add：只要发生在它自己 Done 之前，计数就不会提前归零。
void TestDynamicAdd()
{
    WaitGroup wg;
    std::atomic<int> total{0};
    wg.Add(1);
    std::vector<Thread> ts(4);
    std::atomic<int> started{0};
    Thread parent;
    CHECK(parent.Start(
              [&]
              {
                  for (auto& t : ts)
                  {
                      wg.Add(1); // 先 Add 再启动，自己的 Done 还没到
                      CHECK(t.Start(
                                [&]
                                {
                                    total.fetch_add(1);
                                    wg.Done();
                                }) == 0);
                      started.fetch_add(1);
                  }
                  wg.Done();
              }) == 0);
    wg.Wait();
    CHECK(started.load() == 4 && total.load() == 4);
    parent.Join();
    for (auto& t : ts)
        t.Join();
}

void TestWaitForAndUntil()
{
    WaitGroup wg;
    wg.Add(1);
    auto t0 = std::chrono::steady_clock::now();
    CHECK(!wg.WaitFor(50ms)); // 超时
    CHECK(std::chrono::steady_clock::now() - t0 >= 45ms);
    CHECK(!wg.WaitUntil(std::chrono::steady_clock::now() + 20ms));

    Thread t;
    CHECK(t.Start(
              [&]
              {
                  std::this_thread::sleep_for(30ms);
                  wg.Done();
              }) == 0);
    CHECK(wg.WaitFor(5s)); // 在期限内归零
    t.Join();
    CHECK(wg.WaitFor(0ms));
    CHECK(wg.WaitUntil(std::chrono::steady_clock::now()));
}

// 多个等待者同时被放行。
void TestManyWaiters()
{
    WaitGroup wg;
    wg.Add(1);
    std::atomic<int> released{0};
    std::vector<Thread> ts(6);
    for (auto& t : ts)
        CHECK(t.Start(
                  [&]
                  {
                      wg.Wait();
                      released.fetch_add(1);
                  }) == 0);
    std::this_thread::sleep_for(50ms);
    CHECK(released.load() == 0);
    wg.Done();
    CHECK(WaitUntil([&] { return released.load() == 6; }));
    for (auto& t : ts)
        t.Join();
}

// Wait 返回之后立刻销毁对象是安全的：Done 在持锁时发出通知，等待者拿到锁时 Done 已经不再碰对象。
// 反复做，asan / tsan 下能抓到"通知时对象已销毁"。
void TestDestroyRightAfterWait()
{
    for (int i = 0; i < 3000; ++i)
    {
        Thread t;
        {
            WaitGroup wg;
            wg.Add(1);
            CHECK(t.Start([&wg] { wg.Done(); }) == 0);
            wg.Wait();
        } // wg 在这里销毁，而 Done 的线程可能刚刚返回
        t.Join();
    }
}

// Done 多于 Add：编程错误，FATAL。（老实现用无符号数，回绕成 4294967295，之后 Wait 永远不返回。）
void TestNegativeCountIsFatal()
{
    CHECK(ExpectAbort(
        []
        {
            WaitGroup wg;
            wg.Done();
        }));
    CHECK(ExpectAbort(
        []
        {
            WaitGroup wg;
            wg.Add(2);
            wg.Add(-3);
        }));
}

} // namespace

int main()
{
    TestBasic();
    TestReuse();
    TestDynamicAdd();
    TestWaitForAndUntil();
    TestManyWaiters();
    TestDestroyRightAfterWait();
    TestNegativeCountIsFatal();
    std::puts("wait_group_test: ok");
    return 0;
}
