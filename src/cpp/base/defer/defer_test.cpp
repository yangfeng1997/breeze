// Defer：执行时机与顺序、Dismiss / Run、移动后只执行一次、异常展开时也执行、宏的唯一变量名。
// 无测试框架依赖：失败即 abort。

#include "defer.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
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

void TestRunsAtScopeExitInReverseOrder()
{
    std::string log;
    {
        DEFER(log += 'a');
        DEFER(log += 'b');
        DEFER({
            log += 'c';
            log += 'd';
        });                 // 多语句
        CHECK(log.empty()); // 还没离开作用域
    }
    CHECK(log == "cdba");
}

void TestCapturesByReference()
{
    int v = 1;
    int seen = 0;
    {
        DEFER(seen = v);
        v = 42; // 回调执行时才读，看到的是最终值
    }
    CHECK(seen == 42);
}

// 同一行放两个 DEFER：用 __LINE__ 做变量名会重名，这里用 __COUNTER__。
#define TWO_DEFERS_ON_ONE_LINE(log)                                                                                    \
    DEFER(log += '1');                                                                                                 \
    DEFER(log += '2')

void TestTwoOnOneLine()
{
    std::string log;
    {
        TWO_DEFERS_ON_ONE_LINE(log);
    }
    CHECK(log == "21");
}

void TestRunsEveryIteration()
{
    int n = 0;
    for (int i = 0; i < 5; ++i)
    {
        DEFER(++n);
        if (i == 2)
            continue; // 提前离开本次迭代，DEFER 照样执行
    }
    CHECK(n == 5);
}

void TestEarlyReturn()
{
    int n = 0;
    auto f = [&](bool early)
    {
        DEFER(++n);
        if (early)
            return 1;
        return 2;
    };
    CHECK(f(true) == 1 && n == 1);
    CHECK(f(false) == 2 && n == 2);
}

void TestRunsOnException()
{
    int n = 0;
    try
    {
        DEFER(++n);
        throw std::runtime_error("x");
    }
    catch (const std::runtime_error&)
    {
        CHECK(n == 1); // 栈展开时守卫已经析构
    }
}

void TestDismiss()
{
    int n = 0;
    {
        auto g = MakeDefer([&] { ++n; });
        CHECK(g.active());
        g.Dismiss();
        CHECK(!g.active());
    }
    CHECK(n == 0);
}

void TestRunNow()
{
    int n = 0;
    {
        auto g = MakeDefer([&] { ++n; });
        g.Run();
        CHECK(n == 1 && !g.active());
        g.Run(); // 再调什么也不做
        CHECK(n == 1);
    } // 析构也不再执行
    CHECK(n == 1);
}

void TestMoveRunsOnce()
{
    int n = 0;
    {
        auto g1 = MakeDefer([&] { ++n; });
        {
            auto g2 = std::move(g1);
            CHECK(!g1.active() && g2.active()); // NOLINT：移走后只读 active()
        }
        CHECK(n == 1); // g2 析构时执行
    }
    CHECK(n == 1); // g1 析构不再执行
}

// 回调是只能移动的对象（捕获 unique_ptr）：直接存放，不经过要求可拷贝的 std::function。
void TestMoveOnlyCallable()
{
    int freed = 0;
    {
        auto p = std::make_unique<int>(7);
        auto g = MakeDefer([&freed, p = std::move(p)] { freed = *p; });
    }
    CHECK(freed == 7);
}

struct Counter
{
    int* n;
    void operator()() const { ++*n; }
};

void TestFunctorAndCtad()
{
    int n = 0;
    {
        Defer g{Counter{&n}}; // 类模板参数推导
        auto g2 = MakeDefer(Counter{&n});
    }
    CHECK(n == 2);
}

// 守卫对象本身不占堆：大小就是回调 + 一个 bool（对齐后）。
void TestNoHeapOrErasure()
{
    int a = 0;
    auto g = MakeDefer([&a] { ++a; });
    static_assert(sizeof(g) <= sizeof(void*) * 2);
    g.Dismiss();
}

} // namespace

int main()
{
    TestRunsAtScopeExitInReverseOrder();
    TestCapturesByReference();
    TestTwoOnOneLine();
    TestRunsEveryIteration();
    TestEarlyReturn();
    TestRunsOnException();
    TestDismiss();
    TestRunNow();
    TestMoveRunsOnce();
    TestMoveOnlyCallable();
    TestFunctorAndCtad();
    TestNoHeapOrErasure();
    std::puts("defer_test: ok");
    return 0;
}
