// 无测试框架依赖：失败即 abort。并发用例带超时，卡死也会 abort 而不是挂住 ctest。
// 想抓数据竞争，用 tsan 预设再跑一遍（cmake --preset tsan）。

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <thread>
#include <vector>

#include "mpsc_queue.h"
#include "spsc_queue.h"

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

using Clock = std::chrono::steady_clock;

constexpr auto kDeadline = std::chrono::seconds(60);

// ---- SPSC ----

void TestSpscBasic()
{
    SpscQueue<int> q;
    int v = -1;
    CHECK(!q.TryDequeue(v)); // 空队列
    q.Enqueue(1);
    q.Enqueue(2);
    q.Enqueue(3);
    CHECK(q.SizeApprox() == 3);
    CHECK(q.TryDequeue(v) && v == 1);
    CHECK(q.TryDequeue(v) && v == 2);
    CHECK(q.TryDequeue(v) && v == 3);
    CHECK(!q.TryDequeue(v));
}

// TryEnqueue 只在预分配空间内成功，失败不是容量上限：Enqueue 照样能继续，且顺序不乱。
void TestSpscTryEnqueueIsNotCapacity()
{
    SpscQueue<int> q(3);
    int n = 0;
    while (q.TryEnqueue(n))
    {
        ++n;
        CHECK(n < (1 << 20)); // 一直成功就说明它不是"无预分配"语义
    }
    CHECK(n >= 1);
    for (int i = n; i < n + 100; ++i)
        q.Enqueue(i); // 无界，不失败
    int v = -1;
    for (int i = 0; i < n + 100; ++i)
        CHECK(q.TryDequeue(v) && v == i);
    CHECK(!q.TryDequeue(v));
}

// n < limit ⇒ 队列已空；n == limit 时不保证已空。
void TestSpscBulkContract()
{
    SpscQueue<int> q;
    for (int i = 0; i < 10; ++i)
        q.Enqueue(i);
    std::vector<int> out;
    CHECK(q.TryDequeueBulk(std::back_inserter(out), 4) == 4);
    CHECK(q.TryDequeueBulk(std::back_inserter(out), 4) == 4);
    CHECK(q.TryDequeueBulk(std::back_inserter(out), 4) == 2); // 2 < 4 ⇒ 已空
    CHECK(q.TryDequeueBulk(std::back_inserter(out), 4) == 0);
    CHECK(out.size() == 10);
    for (int i = 0; i < 10; ++i)
        CHECK(out[static_cast<std::size_t>(i)] == i);
}

void TestSpscMoveOnly()
{
    SpscQueue<std::unique_ptr<int>> q;
    q.Enqueue(std::make_unique<int>(7));
    q.Enqueue(std::make_unique<int>(8));
    std::unique_ptr<int> p;
    CHECK(q.TryDequeue(p) && *p == 7);
    std::vector<std::unique_ptr<int>> out;
    CHECK(q.TryDequeueBulk(std::back_inserter(out), 8) == 1); // 1 < 8 ⇒ 已空
    CHECK(*out[0] == 8);
}

void TestSpscPeak()
{
    SpscQueue<int> q;
    CHECK(q.PeakApprox() == 0);
    for (int i = 0; i < 5; ++i)
        q.Enqueue(i);
    std::vector<int> out;
    CHECK(q.TryDequeueBulk(std::back_inserter(out), 2) == 2);
    CHECK(q.PeakApprox() == 5); // 出队前采样，看到的是 5
    CHECK(q.TryDequeueBulk(std::back_inserter(out), 10) == 3);
    CHECK(q.PeakApprox() == 5); // 峰值只升不降
}

void TestSpscStress()
{
    constexpr int kCount = 200000;
    SpscQueue<int> q;
    std::thread producer(
        [&]
        {
            for (int i = 0; i < kCount; ++i)
                q.Enqueue(i);
        });
    const auto deadline = Clock::now() + kDeadline;
    std::vector<int> batch;
    int expect = 0;
    while (expect < kCount)
    {
        batch.clear();
        const std::size_t n = q.TryDequeueBulk(std::back_inserter(batch), 64);
        for (std::size_t i = 0; i < n; ++i)
            CHECK(batch[i] == expect++); // 不丢、不乱序
        if (n == 0)
        {
            CHECK(Clock::now() < deadline);
            std::this_thread::yield();
        }
    }
    producer.join();
    int v = 0;
    CHECK(!q.TryDequeue(v));
}

// ---- MPSC ----

void TestMpscBasic()
{
    MpscQueue<int> q;
    int v = -1;
    CHECK(!q.TryDequeue(v));
    q.Enqueue(1);
    q.Enqueue(2);
    CHECK(q.TryDequeue(v) && v == 1); // 单个生产者内部有序
    CHECK(q.TryDequeue(v) && v == 2);
    CHECK(!q.TryDequeue(v));
}

void TestMpscTryEnqueue()
{
    MpscQueue<int> q(1024);
    CHECK(q.TryEnqueue(42)); // 预分配范围内
    int v = 0;
    CHECK(q.TryDequeue(v) && v == 42);
}

void TestMpscBulkContract()
{
    MpscQueue<int> q;
    for (int i = 0; i < 10; ++i)
        q.Enqueue(i);
    std::vector<int> out;
    CHECK(q.TryDequeueBulk(std::back_inserter(out), 4) == 4);
    CHECK(q.TryDequeueBulk(std::back_inserter(out), 4) == 4);
    CHECK(q.TryDequeueBulk(std::back_inserter(out), 4) == 2); // 2 < 4 ⇒ 已空
    CHECK(q.TryDequeueBulk(std::back_inserter(out), 4) == 0);
    for (int i = 0; i < 10; ++i)
        CHECK(out[static_cast<std::size_t>(i)] == i);
}

void TestMpscMoveOnlyAndToken()
{
    MpscQueue<std::unique_ptr<int>> q;
    auto tok = q.MakeProducerToken();
    q.Enqueue(tok, std::make_unique<int>(1));
    q.Enqueue(std::make_unique<int>(2)); // 隐式生产者与 token 混用
    std::vector<std::unique_ptr<int>> out;
    CHECK(q.TryDequeueBulk(std::back_inserter(out), 8) == 2);
    int sum = 0;
    for (const auto& p : out)
        sum += *p;
    CHECK(sum == 3);
}

void TestMpscPeak()
{
    MpscQueue<int> q;
    for (int i = 0; i < 5; ++i)
        q.Enqueue(i);
    std::vector<int> out;
    CHECK(q.TryDequeueBulk(std::back_inserter(out), 10) == 5);
    CHECK(q.PeakApprox() == 5);
}

struct Item
{
    int producer;
    int seq;
};

// 多生产者各自内部有序且不丢；一半生产者用 token，一半用隐式生产者。
void TestMpscPerProducerOrder()
{
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 50000;
    MpscQueue<Item> q;

    std::vector<std::thread> threads;
    for (int p = 0; p < kProducers; ++p)
    {
        threads.emplace_back(
            [&q, p]
            {
                if (p % 2 == 0)
                {
                    auto tok = q.MakeProducerToken();
                    for (int i = 0; i < kPerProducer; ++i)
                        q.Enqueue(tok, Item{p, i});
                }
                else
                {
                    for (int i = 0; i < kPerProducer; ++i)
                        q.Enqueue(Item{p, i});
                }
            });
    }

    std::array<int, kProducers> next{};
    const auto deadline = Clock::now() + kDeadline;
    std::vector<Item> batch;
    int received = 0;
    while (received < kProducers * kPerProducer)
    {
        batch.clear();
        const std::size_t n = q.TryDequeueBulk(std::back_inserter(batch), 64);
        for (const Item& it : batch)
        {
            CHECK(it.seq == next[static_cast<std::size_t>(it.producer)]++); // 每个生产者内部连续
            ++received;
        }
        if (n == 0)
        {
            CHECK(Clock::now() < deadline);
            std::this_thread::yield();
        }
    }
    for (auto& t : threads)
        t.join();
    Item it{};
    CHECK(!q.TryDequeue(it));
}

} // namespace

int main()
{
    TestSpscBasic();
    TestSpscTryEnqueueIsNotCapacity();
    TestSpscBulkContract();
    TestSpscMoveOnly();
    TestSpscPeak();
    TestSpscStress();

    TestMpscBasic();
    TestMpscTryEnqueue();
    TestMpscBulkContract();
    TestMpscMoveOnlyAndToken();
    TestMpscPeak();
    TestMpscPerProducerOrder();

    std::puts("queue_test: ok");
    return 0;
}
