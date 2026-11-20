// LazyQueue：空的时候不分配、第一次入队才分配、弹空即释放、保序、释放之后能再用、能装只移动的元素。
// 用全局 operator new / delete 的计数证明"不分配 / 已释放"。无测试框架依赖：失败即 abort。

#include "base/net/stream/internal/lazy_queue.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <vector>

#define CHECK(...)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #__VA_ARGS__);                       \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

namespace
{
std::atomic<long> g_allocs{0};
std::atomic<long> g_frees{0};
long Live() { return g_allocs.load() - g_frees.load(); }
} // namespace

void* operator new(std::size_t n)
{
    ++g_allocs;
    if (void* p = std::malloc(n))
        return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept
{
    if (p != nullptr)
        ++g_frees;
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }

using breeze::LazyQueue;

namespace
{

void TestEmptyDoesNotAllocate()
{
    const long before = g_allocs.load();
    {
        LazyQueue<std::string> q;
        CHECK(q.empty() && q.size() == 0);
    }
    CHECK(g_allocs.load() == before); // 构造、查询、析构都没有分配
}

void TestFirstPushAllocatesAndLastPopFrees()
{
    const long live0 = Live();
    LazyQueue<int> q;
    q.emplace_back(1);
    CHECK(!q.empty() && q.size() == 1);
    CHECK(Live() > live0); // 第一次入队才分配
    q.emplace_back(2);
    q.pop_front();
    CHECK(!q.empty() && Live() > live0); // 还剩一个：不释放
    q.pop_front();
    CHECK(q.empty() && q.size() == 0);
    CHECK(Live() == live0); // 弹空即释放，没有残留
}

void TestFifoOrderAndIteration()
{
    const long live0 = Live();
    LazyQueue<int> q;
    for (int i = 0; i < 1000; ++i) // 跨好几个 deque 节点
        q.emplace_back(i);
    CHECK(q.size() == 1000);
    int expect = 0;
    for (auto it = q.begin(); it != q.end(); ++it)
        CHECK(*it == expect++);
    for (int i = 0; i < 1000; ++i)
    {
        CHECK(q.front() == i);
        q.pop_front();
    }
    CHECK(q.empty());
    CHECK(Live() == live0);
}

// 弹空释放之后继续用：反复"积压一点又排空"。
void TestReuseAfterRelease()
{
    const long live0 = Live();
    LazyQueue<std::string> q;
    for (int round = 0; round < 1000; ++round)
    {
        q.emplace_back("a" + std::to_string(round));
        q.emplace_back("b" + std::to_string(round));
        q.emplace_back("c" + std::to_string(round));
        CHECK(q.front() == "a" + std::to_string(round));
        q.pop_front();
        CHECK(q.front() == "b" + std::to_string(round));
        q.pop_front();
        CHECK(q.front() == "c" + std::to_string(round));
        q.pop_front();
        CHECK(q.empty());
    }
    CHECK(Live() == live0);
}

// emplace_back 返回新元素的引用，随即可以就地填充（连接里就是这么用的）。
void TestEmplaceReturnsReferenceToTheNewElement()
{
    struct Item
    {
        int a = 0;
        std::string s;
    };
    LazyQueue<Item> q;
    Item& x = q.emplace_back();
    x.a = 7;
    x.s = "seven";
    q.emplace_back().a = 8;
    CHECK(q.front().a == 7 && q.front().s == "seven");
    q.pop_front();
    CHECK(q.front().a == 8);
}

void TestMoveOnlyElement()
{
    const long live0 = Live();
    {
        LazyQueue<std::unique_ptr<int>> q;
        q.emplace_back(std::make_unique<int>(41));
        q.emplace_back(std::make_unique<int>(42));
        CHECK(*q.front() == 41);
        q.pop_front();
        CHECK(*q.front() == 42);
        // 不弹空就析构：不泄漏
    }
    CHECK(Live() == live0);
}

} // namespace

int main()
{
    TestEmptyDoesNotAllocate();
    TestFirstPushAllocatesAndLastPopFrees();
    TestFifoOrderAndIteration();
    TestReuseAfterRelease();
    TestEmplaceReturnsReferenceToTheNewElement();
    TestMoveOnlyElement();
    std::puts("lazy_queue_test: ok");
    return 0;
}
