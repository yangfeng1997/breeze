// SpinLock：互斥、try_lock、与标准库的 Guard 配合、线程数多于核数时仍然能做完（自旋之后让出 CPU）。

#include "base/thread/spin_lock.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include "base/thread/thread.h"
#include "base/thread/thread_test_util.h"

using namespace breeze;
using namespace breeze::testing;

namespace
{

// 非原子的计数器在锁里递增：互斥失败就会丢更新（tsan 下还会直接报数据竞争）。
void RunCounter(int threads, int iterations, bool yield_inside)
{
    SpinLock lock;
    long counter = 0;
    std::vector<Thread> ts(static_cast<std::size_t>(threads));
    std::atomic<bool> go{false};
    for (auto& t : ts)
    {
        CHECK(t.Start(
                  [&]
                  {
                      while (!go.load(std::memory_order_acquire))
                          std::this_thread::yield();
                      for (int i = 0; i < iterations; ++i)
                      {
                          std::lock_guard g(lock);
                          const long v = counter;
                          if (yield_inside && i % 16 == 0)
                              std::this_thread::yield(); // 持锁时被换出去：别的线程在这期间自旋
                          counter = v + 1;
                      }
                  }) == 0);
    }
    go = true;
    for (auto& t : ts)
        t.Join();
    CHECK(counter == static_cast<long>(threads) * iterations);
}

void TestMutualExclusion() { RunCounter(8, 100000, false); }

// 线程数是核数的好几倍、持锁者还会被换出去：纯忙等会把时间片烧光（持锁者拿不到 CPU 释放锁），
// 自旋一阵之后 sched_yield 才能做完。设了时限，卡住会 abort。
void TestOversubscribed()
{
    const int cores = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    const int threads = std::min(cores * 4, 128);
    const auto t0 = std::chrono::steady_clock::now();
    RunCounter(threads, 2000, true);
    CHECK(std::chrono::steady_clock::now() - t0 < 30s);
}

void TestTryLock()
{
    SpinLock lock;
    CHECK(lock.try_lock());
    CHECK(!lock.try_lock()); // 已被持有（同一个线程也不可重入）
    bool other = true;
    Thread t;
    CHECK(t.Start([&] { other = lock.try_lock(); }) == 0);
    t.Join();
    CHECK(!other);
    lock.unlock();
    CHECK(lock.try_lock());
    lock.unlock();
}

// 满足 BasicLockable / Lockable：标准库的 Guard 都能用，不需要自己的 Guard 类。
void TestWorksWithStdGuards()
{
    SpinLock a;
    SpinLock b;
    {
        std::unique_lock<SpinLock> u(a);
        CHECK(u.owns_lock());
        CHECK(!a.try_lock());
        u.unlock();
        CHECK(a.try_lock());
        a.unlock();
    }
    {
        std::scoped_lock both(a, b); // 同时锁两把（死锁避免算法要用 try_lock）
        CHECK(!a.try_lock() && !b.try_lock());
    }
    CHECK(a.try_lock() && b.try_lock());
    a.unlock();
    b.unlock();
    std::unique_lock<SpinLock> tried(a, std::try_to_lock);
    CHECK(tried.owns_lock());
}

} // namespace

int main()
{
    TestMutualExclusion();
    TestOversubscribed();
    TestTryLock();
    TestWorksWithStdGuards();
    std::puts("spin_lock_test: ok");
    return 0;
}
