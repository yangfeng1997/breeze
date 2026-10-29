// ThreadPool：Post / Submit / key 串行、共享队列不被长任务堵住、队列上限、Stop
// 排空（含任务里再投递）、停机期间拒绝外部提交、 各种误用即 FATAL，以及多生产者压力。无测试框架依赖：失败即
// abort，并发用例带超时。
//
// 用法：thread_pool_test           跑全部用例
//       thread_pool_test --bench   只测吞吐（不进 ctest），数字记在 docs/线程基础库.md

#include "thread_pool.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <latch>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "current_thread.h"
#include "thread.h"
#include "thread_test_util.h"

using namespace breeze;
using namespace breeze::testing;

namespace
{

ThreadPoolOptions Opt(std::size_t threads, std::size_t max_pending = 0, std::string prefix = "Worker")
{
    ThreadPoolOptions o;
    o.threads = threads;
    o.name_prefix = std::move(prefix);
    o.max_pending = max_pending;
    return o;
}

// 一个能被测试线程放行的"阻塞任务"：Post 之后先等 started，需要时 Release。
struct Blocker
{
    std::atomic<bool> started{false};
    std::atomic<bool> release{false};
    void operator()()
    {
        started = true;
        while (!release.load())
            std::this_thread::sleep_for(100us);
    }
    ThreadPool::Task AsTask()
    {
        return [this] { (*this)(); };
    }
};

void TestPostAndWaitIdle()
{
    ThreadPool pool(Opt(4));
    CHECK(pool.Start() == 0);
    CHECK(pool.running() && pool.thread_count() == 4);

    std::atomic<int> n{0};
    constexpr int kN = 10000;
    for (int i = 0; i < kN; ++i)
        CHECK(pool.Post([&] { n.fetch_add(1); }));
    pool.WaitIdle();
    CHECK(n.load() == kN);
    const auto s = pool.stats();
    CHECK(s.executed == kN && s.pending == 0 && s.rejected == 0 && s.threads == 4);

    pool.Stop();
    CHECK(!pool.running());
    pool.Stop(); // 重复：空操作
}

void TestSubmitResultsAndArgs()
{
    ThreadPool pool(Opt(3));
    CHECK(pool.Start() == 0);

    auto a = pool.Submit([] { return 6 * 7; });
    CHECK(a.has_value() && a->get() == 42);

    auto b = pool.Submit([](int x, std::string s) { return s + std::to_string(x); }, 5, std::string("v"));
    CHECK(b.has_value() && b->get() == "v5");

    auto c =
        pool.Submit([](std::unique_ptr<int> p) { return *p + 1; }, std::make_unique<int>(9)); // 参数可以是只能移动的
    CHECK(c.has_value() && c->get() == 10);

    std::atomic<int> side{0};
    auto d = pool.Submit([&] { side.store(1); });
    CHECK(d.has_value());
    d->get();
    CHECK(side.load() == 1);

    auto e = pool.SubmitKey(7, [] { return this_thread::Tid(); });
    auto f = pool.SubmitKey(7, [] { return this_thread::Tid(); });
    CHECK(e.has_value() && f.has_value() && e->get() == f->get()); // 同 key 同一个线程

    // 多个 future 并发等
    std::vector<std::future<int>> futs;
    for (int i = 0; i < 200; ++i)
        futs.push_back(*pool.Submit([i] { return i * 2; }));
    for (int i = 0; i < 200; ++i)
        CHECK(futs[static_cast<std::size_t>(i)].get() == i * 2);
}

// 没在运行（没 Start、Stop 之后）：拒绝而不是崩溃（老实现这里是对 0 取模）；被拒绝的任务已销毁，不泄漏。
void TestRejectedWhenNotRunning()
{
    const auto token = std::make_shared<int>(0);
    ThreadPool pool(Opt(2));
    CHECK(!pool.Post([token] {}));
    CHECK(token.use_count() == 1);
    CHECK(!pool.PostKey(3, [token] {}));
    CHECK(!pool.Submit([] { return 1; }).has_value());
    CHECK(!pool.SubmitKey(3, [] { return 1; }).has_value());
    CHECK(pool.stats().rejected >= 2);

    CHECK(pool.Start() == 0);
    CHECK(pool.Post([] {}));
    pool.Stop();
    CHECK(!pool.Post([token] {}));
    CHECK(token.use_count() == 1);
    CHECK(!pool.Submit([] { return 1; }).has_value());
    CHECK(pool.stats().rejected >= 5);
}

void TestStartRejectsBadThreadCount()
{
    ThreadPool zero(Opt(0));
    CHECK(zero.Start() == EINVAL);
    CHECK(!zero.running());
    ThreadPool many(Opt(ThreadPool::kMaxThreads + 1));
    CHECK(many.Start() == EINVAL);
    ThreadPool max(Opt(ThreadPool::kMaxThreads));
    CHECK(max.Start() == 0); // 边界值合法
    max.Stop();
}

// 同 key 的任务在同一个线程上按提交顺序串行执行；key % 线程数相同的 key 落在同一个线程。
void TestKeyedOrderAndAffinity()
{
    constexpr std::size_t kThreads = 4;
    constexpr int kKeys = 16;
    constexpr int kPerKey = 500;
    ThreadPool pool(Opt(kThreads));
    CHECK(pool.Start() == 0);

    std::vector<std::vector<int>> seen(kKeys); // 每个 key 一个，只有它所在的线程会写
    std::vector<std::atomic<pid_t>> tid_of(kKeys);
    for (auto& t : tid_of)
        t = 0;
    std::atomic<int> violations{0};
    for (int n = 0; n < kPerKey; ++n)
    {
        for (int k = 0; k < kKeys; ++k)
        {
            CHECK(pool.PostKey(static_cast<std::uint64_t>(k),
                               [&, k, n]
                               {
                                   auto& v = seen[static_cast<std::size_t>(k)];
                                   if (!v.empty() && v.back() != n - 1)
                                       violations.fetch_add(1);
                                   v.push_back(n);
                                   pid_t expected = 0;
                                   const pid_t me = this_thread::Tid();
                                   if (!tid_of[static_cast<std::size_t>(k)].compare_exchange_strong(expected, me) &&
                                       expected != me)
                                       violations.fetch_add(1);
                               }));
        }
    }
    pool.WaitIdle();
    CHECK(violations.load() == 0);
    for (int k = 0; k < kKeys; ++k)
        CHECK(seen[static_cast<std::size_t>(k)].size() == kPerKey);
    for (int a = 0; a < kKeys; ++a)
        for (int b = 0; b < kKeys; ++b)
            if (a % kThreads == b % kThreads)
                CHECK(tid_of[static_cast<std::size_t>(a)].load() == tid_of[static_cast<std::size_t>(b)].load());
}

// 一个线程被长任务占着，别的任务不能一直排在它后面。老实现的"轮询"是盲分配，被占线程的队列里的任务只能干等。
void TestSharedQueueIsNotBlockedByBusyWorker()
{
    ThreadPool pool(Opt(2));
    CHECK(pool.Start() == 0);
    Blocker blocker;
    CHECK(pool.Post(blocker.AsTask()));
    CHECK(WaitUntil([&] { return blocker.started.load(); }));

    std::atomic<int> n{0};
    for (int i = 0; i < 500; ++i)
        CHECK(pool.Post([&] { n.fetch_add(1); }));
    CHECK(WaitUntil([&] { return n.load() == 500; })); // 阻塞任务还没放行：全部由另一个线程做完
    blocker.release = true;
    pool.WaitIdle();
}

// 有 key 的任务只会堵住自己所在线程的 key，不影响共享队列。
void TestKeyedBacklogDoesNotBlockSharedQueue()
{
    ThreadPool pool(Opt(2));
    CHECK(pool.Start() == 0);
    Blocker blocker;
    CHECK(pool.PostKey(0, blocker.AsTask())); // 占住 0 号线程
    CHECK(WaitUntil([&] { return blocker.started.load(); }));

    std::atomic<int> shared{0};
    std::atomic<int> k1{0};
    std::atomic<int> k0{0};
    for (int i = 0; i < 100; ++i)
    {
        CHECK(pool.Post([&] { shared.fetch_add(1); }));
        CHECK(pool.PostKey(1, [&] { k1.fetch_add(1); }));
        CHECK(pool.PostKey(0, [&] { k0.fetch_add(1); }));
    }
    CHECK(WaitUntil([&] { return shared.load() == 100 && k1.load() == 100; }));
    CHECK(k0.load() == 0); // key 0 的任务排在阻塞任务后面
    blocker.release = true;
    pool.WaitIdle();
    CHECK(k0.load() == 100);
}

void TestBoundedQueue()
{
    ThreadPool pool(Opt(1, /*max_pending=*/10));
    CHECK(pool.Start() == 0);
    Blocker blocker;
    CHECK(pool.Post(blocker.AsTask())); // 在运行，不算排队
    CHECK(WaitUntil([&] { return blocker.started.load(); }));

    std::atomic<int> n{0};
    for (int i = 0; i < 10; ++i)
        CHECK(pool.Post([&] { n.fetch_add(1); }));
    CHECK(pool.stats().pending == 10);
    CHECK(!pool.Post([&] { n.fetch_add(100); })); // 满了
    CHECK(!pool.PostKey(0, [&] { n.fetch_add(100); }));
    CHECK(!pool.Submit([] { return 1; }).has_value());
    CHECK(pool.stats().rejected == 3);

    blocker.release = true;
    pool.WaitIdle();
    CHECK(n.load() == 10);                     // 被拒绝的没有执行
    CHECK(pool.Post([&] { n.fetch_add(1); })); // 腾出空间后又能提交
    pool.WaitIdle();
    CHECK(n.load() == 11);
}

// Stop 把已经排队的全部做完，而不是丢掉。
void TestStopDrainsQueuedTasks()
{
    std::atomic<int> n{0};
    {
        ThreadPool pool(Opt(4));
        CHECK(pool.Start() == 0);
        for (int i = 0; i < 20000; ++i)
            CHECK(pool.Post([&] { n.fetch_add(1); }));
        for (int i = 0; i < 20000; ++i)
            CHECK(pool.PostKey(static_cast<std::uint64_t>(i), [&] { n.fetch_add(1); }));
        pool.Stop();
        CHECK(n.load() == 40000);
    }
    // 析构兜底：没 Stop 也会排空
    std::atomic<int> m{0};
    {
        ThreadPool pool(Opt(2));
        CHECK(pool.Start() == 0);
        for (int i = 0; i < 5000; ++i)
            CHECK(pool.Post([&] { m.fetch_add(1); }));
    }
    CHECK(m.load() == 5000);
}

// 排空期间任务再投递的，也都会执行（老实现里它们可能落到已经退出的线程队列里，future 变 broken_promise）。
void TestStopAllowsRepostFromTasks()
{
    // 链：每个任务投递下一个
    {
        ThreadPool pool(Opt(3));
        CHECK(pool.Start() == 0);
        std::atomic<int> n{0};
        constexpr int kDepth = 5000;
        std::function<void(int)> step = [&](int i)
        {
            n.fetch_add(1);
            if (i < kDepth)
                CHECK(pool.Post([&step, i] { step(i + 1); })); // 即使在 Stopping 里，工作线程投递也被接受
        };
        CHECK(pool.Post([&] { step(0); }));
        pool.Stop(); // 链还在跑
        CHECK(n.load() == kDepth + 1);
    }
    // 扇出：每个任务投两个孩子（共 2^(d+1)-1 个），混用无 key 与有 key
    {
        ThreadPool pool(Opt(4));
        CHECK(pool.Start() == 0);
        std::atomic<int> n{0};
        std::function<void(int, std::uint64_t)> fan = [&](int depth, std::uint64_t key)
        {
            n.fetch_add(1);
            if (depth == 0)
                return;
            CHECK(pool.Post([&fan, depth, key] { fan(depth - 1, key * 2); }));
            CHECK(pool.PostKey(key + 1, [&fan, depth, key] { fan(depth - 1, key * 2 + 1); }));
        };
        CHECK(pool.Post([&] { fan(10, 1); }));
        pool.Stop();
        CHECK(n.load() == (1 << 11) - 1);
    }
}

// 开始停止之后，外部的新提交被拒绝（工作线程里的再投递除外）。
void TestExternalPostRejectedWhileStopping()
{
    ThreadPool pool(Opt(2));
    CHECK(pool.Start() == 0);
    Blocker blocker;
    CHECK(pool.Post(blocker.AsTask()));
    CHECK(WaitUntil([&] { return blocker.started.load(); }));

    Thread stopper;
    CHECK(stopper.Start([&] { pool.Stop(); }, "stopper") == 0); // 卡在等阻塞任务
    CHECK(WaitUntil([&] { return !pool.running(); }));
    CHECK(!pool.Post([] {}));
    CHECK(!pool.PostKey(1, [] {}));
    CHECK(!pool.Submit([] { return 1; }).has_value());
    blocker.release = true;
    stopper.Join();
    CHECK(pool.stats().rejected == 3);
}

void TestWaitIdle()
{
    ThreadPool pool(Opt(4));
    CHECK(pool.Start() == 0);
    pool.WaitIdle(); // 什么都没提交：立即返回
    std::atomic<int> n{0};
    for (int i = 0; i < 8; ++i)
        CHECK(pool.Post(
            [&]
            {
                std::this_thread::sleep_for(20ms);
                n.fetch_add(1);
            }));
    pool.WaitIdle();
    CHECK(n.load() == 8);
    CHECK(pool.stats().pending == 0);
}

void TestWorkerNames()
{
    ThreadPool pool(Opt(3, 0, "Pool"));
    CHECK(pool.Start() == 0);
    CHECK(!pool.InWorkerThread());
    std::mutex mu;
    std::set<std::string> names;
    std::atomic<int> in_worker{0};
    for (int k = 0; k < 3; ++k)
    {
        CHECK(pool.PostKey(static_cast<std::uint64_t>(k),
                           [&]
                           {
                               const std::lock_guard lk(mu);
                               names.insert(this_thread::Name());
                               in_worker.fetch_add(pool.InWorkerThread() ? 1 : 0);
                           }));
    }
    pool.WaitIdle();
    CHECK((names == std::set<std::string>{"Pool-0", "Pool-1", "Pool-2"}));
    CHECK(in_worker.load() == 3);
}

// 被拒绝 / 已执行的任务都会销毁它捕获的状态；只能移动的任务可以提交。
void TestTaskLifetime()
{
    const auto token = std::make_shared<int>(0);
    {
        ThreadPool pool(Opt(2));
        CHECK(pool.Start() == 0);
        for (int i = 0; i < 100; ++i)
            CHECK(pool.Post([token] {}));
        pool.WaitIdle();
        CHECK(token.use_count() == 1); // 任务做完就在锁外销毁了
        auto p = std::make_unique<int>(5);
        std::atomic<int> got{0};
        CHECK(pool.Post([&got, p = std::move(p)] { got = *p; }));
        pool.WaitIdle();
        CHECK(got.load() == 5);
    }
}

// 多个生产者、混用 Post / PostKey：一个都不丢、不重复。
void TestStress()
{
    ThreadPool pool(Opt(4));
    CHECK(pool.Start() == 0);
    constexpr int kProducers = 4;
    constexpr int kPer = 50000;
    std::atomic<std::uint64_t> sum{0};
    std::vector<Thread> producers(kProducers);
    for (int p = 0; p < kProducers; ++p)
    {
        CHECK(producers[static_cast<std::size_t>(p)].Start(
                  [&, p]
                  {
                      for (int i = 0; i < kPer; ++i)
                      {
                          const std::uint64_t v = static_cast<std::uint64_t>(p) * kPer + static_cast<std::uint64_t>(i);
                          const bool ok = (i % 3 == 0) ? pool.PostKey(v, [&sum, v] { sum.fetch_add(v); })
                                                       : pool.Post([&sum, v] { sum.fetch_add(v); });
                          CHECK(ok);
                      }
                  }) == 0);
    }
    for (auto& t : producers)
        t.Join();
    pool.WaitIdle();
    constexpr std::uint64_t total = static_cast<std::uint64_t>(kProducers) * kPer;
    CHECK(sum.load() == total * (total - 1) / 2);
    CHECK(pool.stats().executed == total);
}

// 误用即 FATAL。放在最后：fork 要求此刻没有别的线程。
void TestMisuseIsFatal()
{
    CHECK(ExpectAbort(
        []
        {
            ThreadPool p(Opt(1));
            (void)p.Start();
            (void)p.Start(); // 重复 Start
        }));
    CHECK(ExpectAbort(
        []
        {
            ThreadPool p(Opt(1));
            (void)p.Start();
            (void)p.Post([&p] { p.Stop(); }); // 工作线程里 Stop（会 join 自己）
            std::this_thread::sleep_for(5s);
        }));
    CHECK(ExpectAbort(
        []
        {
            ThreadPool p(Opt(1));
            (void)p.Start();
            (void)p.Post([&p] { p.WaitIdle(); }); // 工作线程里 WaitIdle（会等自己）
            std::this_thread::sleep_for(5s);
        }));
    CHECK(ExpectAbort(
        []
        {
            ThreadPool p(Opt(1));
            (void)p.Start();
            (void)p.Post(ThreadPool::Task{}); // 空任务
        }));
}

// ---- 吞吐（不进 ctest）----

void Bench()
{
    using Clock = std::chrono::steady_clock;
    const auto run = [](const char* label, std::size_t workers, int producers, bool keyed)
    {
        ThreadPool pool(Opt(workers));
        CHECK(pool.Start() == 0);
        constexpr int kTotal = 2000000;
        std::atomic<int> n{0};
        const auto t0 = Clock::now();
        std::vector<Thread> ps(static_cast<std::size_t>(producers));
        for (int p = 0; p < producers; ++p)
        {
            CHECK(ps[static_cast<std::size_t>(p)].Start(
                      [&, p]
                      {
                          for (int i = p; i < kTotal; i += producers)
                          {
                              const bool ok = keyed ? pool.PostKey(static_cast<std::uint64_t>(i),
                                                                   [&n] { n.fetch_add(1, std::memory_order_relaxed); })
                                                    : pool.Post([&n] { n.fetch_add(1, std::memory_order_relaxed); });
                              CHECK(ok);
                          }
                      }) == 0);
        }
        for (auto& t : ps)
            t.Join();
        pool.WaitIdle();
        const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
        std::printf(
            "%-34s %zu 个工作线程 / %d 个生产者：%.2f M task/s\n", label, workers, producers, kTotal / secs / 1e6);
    };
    run("Post（共享队列，空任务）", 4, 1, false);
    run("Post（共享队列，空任务）", 4, 4, false);
    run("PostKey（专属队列，空任务）", 4, 1, true);
    run("PostKey（专属队列，空任务）", 4, 4, true);
    run("Post（共享队列，空任务）", 1, 1, false);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--bench") == 0)
    {
        Bench();
        return 0;
    }
    TestPostAndWaitIdle();
    TestSubmitResultsAndArgs();
    TestRejectedWhenNotRunning();
    TestStartRejectsBadThreadCount();
    TestKeyedOrderAndAffinity();
    TestSharedQueueIsNotBlockedByBusyWorker();
    TestKeyedBacklogDoesNotBlockSharedQueue();
    TestBoundedQueue();
    TestStopDrainsQueuedTasks();
    TestStopAllowsRepostFromTasks();
    TestExternalPostRejectedWhileStopping();
    TestWaitIdle();
    TestWorkerNames();
    TestTaskLifetime();
    TestStress();
    TestMisuseIsFatal();
    std::puts("thread_pool_test: ok");
    return 0;
}
