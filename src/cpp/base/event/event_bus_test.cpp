// EventBus：分发语义、回调里的各种重入（订阅 / 退订 / Clear / 嵌套发布）、递归深度、回调抛异常、句柄、生命周期，
// 以及与一个"快照语义"的朴素参考实现的随机对照。
// 这里把断言模式强制成 Release（BREEZE_ASSERT_FATAL=0）：递归超限时走"丢弃并记日志"的路径，才能断言它。
// 无测试框架依赖：失败即 abort。

#define BREEZE_ASSERT_FATAL 0
#include "event_bus.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace breeze;

// 递归超限的用例会让断言库往 stderr 记很多条预期内的错误日志：那段时间把 fd 2 指向 /dev/null，
// CHECK 的失败信息走启动时复制出来的那份真 stderr，不会丢。
int g_real_stderr = -1;

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            ::dprintf(g_real_stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                          \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

struct QuietStderr
{
    QuietStderr()
    {
        saved = ::dup(STDERR_FILENO);
        const int devnull = ::open("/dev/null", O_WRONLY);
        ::dup2(devnull, STDERR_FILENO);
        ::close(devnull);
    }
    ~QuietStderr()
    {
        ::dup2(saved, STDERR_FILENO);
        ::close(saved);
    }
    int saved;
};

namespace
{

struct Login
{
    int player = 0;
};
struct Logout
{
    int player = 0;
};
namespace other
{
struct Login // 同名、不同命名空间：是另一个事件类型
{
    int player = 0;
};
} // namespace other
struct Base
{
    int v = 0;
};
struct Derived : Base
{
};

void TestBasicDispatch()
{
    EventBus bus;
    std::vector<int> got;
    bus.Subscribe<Login>([&](const Login& e) { got.push_back(e.player); });
    CHECK(bus.size() == 1 && bus.SubscriberCount<Login>() == 1 && bus.HasSubscribers<Login>());
    CHECK(bus.SubscriberCount<Logout>() == 0 && !bus.HasSubscribers<Logout>());

    bus.Publish(Login{7});
    bus.Publish(Login{8});
    bus.Publish(Logout{9});       // 没有订阅者的类型：什么也不发生
    bus.Publish(other::Login{1}); // 同名不同命名空间：不是 Login
    CHECK((got == std::vector<int>{7, 8}));

    // 订阅者按订阅顺序被调用
    std::string order;
    bus.Subscribe<Logout>([&](const Logout&) { order += 'a'; });
    bus.Subscribe<Logout>([&](const Logout&) { order += 'b'; });
    bus.Subscribe<Logout>([&](const Logout&) { order += 'c'; });
    bus.Publish(Logout{});
    CHECK(order == "abc");

    // 回调拿到的就是发布的那个对象（引用，不拷贝）
    const Login ev{42};
    const Login* seen = nullptr;
    bus.Subscribe<Login>([&](const Login& e) { seen = &e; });
    bus.Publish(ev);
    CHECK(seen == &ev);

    // 右值事件也能发布
    int sum = 0;
    EventBus b2;
    b2.Subscribe<Login>([&](const Login& e) { sum += e.player; });
    b2.Publish(Login{5});
    CHECK(sum == 5);
}

// 按静态类型分发：发布 Base 只通知订阅 Base 的，Derived 对象当 Base 发布也是一样。
void TestDispatchIsByStaticType()
{
    EventBus bus;
    int base_calls = 0, derived_calls = 0;
    bus.Subscribe<Base>([&](const Base&) { ++base_calls; });
    bus.Subscribe<Derived>([&](const Derived&) { ++derived_calls; });
    Derived d;
    const Base& as_base = d;
    bus.Publish(as_base);
    CHECK(base_calls == 1 && derived_calls == 0);
    bus.Publish(d);
    CHECK(base_calls == 1 && derived_calls == 1);

    // const E 与 E 是同一个类型
    int n = 0;
    bus.Subscribe<const Login>([&](const Login&) { ++n; });
    bus.Publish(Login{});
    CHECK(n == 1 && bus.SubscriberCount<Login>() == 1);
}

void TestHandlerForms()
{
    EventBus bus;
    int n = 0;
    // 函数对象、std::function（左值与右值）、只能移动的 lambda、返回值被忽略的 lambda
    struct Functor
    {
        int* n;
        void operator()(const Login&) const { ++*n; }
    };
    bus.Subscribe<Login>(Functor{&n});
    std::function<void(const Login&)> fn = [&](const Login&) { ++n; };
    bus.Subscribe<Login>(fn);
    bus.Subscribe<Login>(std::move(fn));
    auto p = std::make_unique<int>(10);
    bus.Subscribe<Login>([&n, p = std::move(p)](const Login&) { n += *p; });
    bus.Subscribe<Login>([&](const Login&) { return ++n; });
    bus.Publish(Login{});
    CHECK(n == 1 + 1 + 1 + 10 + 1);

    // 有状态的 mutable 回调：每次发布状态延续
    EventBus b2;
    int calls = 0;
    b2.Subscribe<Login>([&calls, count = 0](const Login&) mutable { calls = ++count; });
    b2.Publish(Login{});
    b2.Publish(Login{});
    b2.Publish(Login{});
    CHECK(calls == 3);
}

void TestUnsubscribe()
{
    EventBus bus;
    int a = 0, b = 0;
    const SubId ida = bus.Subscribe<Login>([&](const Login&) { ++a; });
    const SubId idb = bus.Subscribe<Login>([&](const Login&) { ++b; });
    CHECK(ida != kInvalidSubId && idb != kInvalidSubId && ida != idb);
    CHECK(bus.Unsubscribe(ida));
    bus.Publish(Login{});
    CHECK(a == 0 && b == 1 && bus.size() == 1);
    CHECK(!bus.Unsubscribe(ida));           // 已经退订
    CHECK(!bus.Unsubscribe(kInvalidSubId)); // 无效句柄
    CHECK(!bus.Unsubscribe(0xdeadbeefdeadbeefULL));
    CHECK(bus.Unsubscribe(idb) && bus.empty());
    bus.Publish(Login{});
    CHECK(a == 0 && b == 1);

    // 别的总线的句柄不能误退订这个总线里的订阅（序号是进程内唯一的）
    EventBus x, y;
    int xs = 0, ys = 0;
    const SubId xid = x.Subscribe<Login>([&](const Login&) { ++xs; });
    y.Subscribe<Login>([&](const Login&) { ++ys; });
    CHECK(!y.Unsubscribe(xid));
    y.Publish(Login{});
    CHECK(ys == 1);
    CHECK(x.Unsubscribe(xid));
}

// 旧版的 bug：回调里订阅同一类型，vector 扩容把"正在执行的回调对象"搬走（std::function
// 被搬动，它的捕获物已经不在原处）， 回调后面再读捕获物就是 use-after-free（asan
// 下会报）。现在条目各自在堆上，地址稳定。
void TestSubscribeInsideHandlerDoesNotMoveRunningHandler()
{
    EventBus bus;
    std::string captured(200, 'x'); // 超出 SSO，搬动后旧缓冲会被释放
    int later_calls = 0;
    bus.Subscribe<Login>(
        [&, text = captured](const Login&)
        {
            for (int i = 0; i < 2000; ++i) // 足够让 entries 扩容很多次
                bus.Subscribe<Login>([&](const Login&) { ++later_calls; });
            CHECK(text == captured); // 读取自己的捕获物
        });
    bus.Publish(Login{});
    CHECK(later_calls == 0); // 本次发布只通知发布开始时已经存在的订阅者
    bus.Publish(Login{});
    CHECK(later_calls == 2000);
    CHECK(bus.SubscriberCount<Login>() == 2001 + 2000);
}

void TestUnsubscribeInsideHandler()
{
    EventBus bus;
    std::string order;
    SubId id_b = 0, id_c = 0, id_self = 0;
    bus.Subscribe<Login>(
        [&](const Login&)
        {
            order += 'a';
            bus.Unsubscribe(id_c); // 还没轮到的：不会再被调用（第二次发布时已经退订过，返回 false）
        });
    id_b = bus.Subscribe<Login>(
        [&](const Login&)
        {
            order += 'b';
            bus.Unsubscribe(id_b); // 退订自己：本次回调照常跑完，之后不再调用
            order += 'B';
        });
    id_c = bus.Subscribe<Login>([&](const Login&) { order += 'c'; });
    id_self = bus.Subscribe<Login>([&](const Login&) { order += 'd'; });
    (void)id_self;
    bus.Publish(Login{});
    CHECK(order == "abBd");
    CHECK(bus.SubscriberCount<Login>() == 2); // a、d 还在
    order.clear();
    bus.Publish(Login{});
    CHECK(order == "ad");
}

// 旧版的 bug：Clear 之后（还在发布期间）新订阅的，会被"发布结束时再真正清空"一起清掉。
void TestClearInsideHandler()
{
    EventBus bus;
    std::string order;
    int fresh_calls = 0;
    bus.Subscribe<Login>(
        [&](const Login&)
        {
            order += 'a';
            bus.Clear();
            bus.Subscribe<Login>([&](const Login&) { ++fresh_calls; }); // Clear 之后新订阅的
        });
    bus.Subscribe<Login>([&](const Login&) { order += 'b'; }); // 被 Clear 掉、还没轮到：不被调用
    bus.Publish(Login{});
    CHECK(order == "a" && fresh_calls == 0); // 新订阅的本次也不调用
    CHECK(bus.size() == 1 && bus.SubscriberCount<Login>() == 1);
    bus.Publish(Login{});
    CHECK(fresh_calls == 1); // 下一次起生效：没有被那次 Clear 吞掉
    bus.Clear();
    CHECK(bus.empty());
    bus.Publish(Login{});
    CHECK(fresh_calls == 1);
}

void TestNestedPublish()
{
    EventBus bus;
    std::string order;
    bus.Subscribe<Login>(
        [&](const Login&)
        {
            order += "L(";
            bus.Publish(Logout{});
            order += ")";
        });
    bus.Subscribe<Logout>([&](const Logout&) { order += "o"; });
    bus.Subscribe<Login>([&](const Login&) { order += "l2"; });
    bus.Publish(Login{});
    CHECK(order == "L(o)l2");
}

// 嵌套发布里退订另一个类型的订阅者：旧版只在最外层那个类型的列表上清理，嵌套类型里已取消的条目留着不释放。
// 现在每个类型各自在它的"发布层数"回到 0 时清理，已取消订阅者的捕获物及时释放。
void TestCancelledEntriesAreReleasedPromptly()
{
    EventBus bus;
    auto token = std::make_shared<int>(1);
    SubId victim = bus.Subscribe<Logout>([token](const Logout&) { (void)token; });
    CHECK(token.use_count() == 2);
    bus.Subscribe<Login>(
        [&](const Login&)
        {
            bus.Publish(Logout{}); // 发布 Logout（嵌套）期间退订它的订阅者
        });
    bus.Subscribe<Logout>([&](const Logout&) { bus.Unsubscribe(victim); });
    bus.Publish(Login{});
    // victim 先于退订它的那个订阅者被调用，所以退订发生在它之后；嵌套发布结束时 Logout 的列表被清理
    CHECK(token.use_count() == 1);

    // 不在发布期间退订：立刻释放
    auto token2 = std::make_shared<int>(2);
    const SubId id = bus.Subscribe<Login>([token2](const Login&) { (void)token2; });
    CHECK(token2.use_count() == 2);
    bus.Unsubscribe(id);
    CHECK(token2.use_count() == 1);

    // Clear、析构也一样
    auto token3 = std::make_shared<int>(3);
    {
        EventBus b2;
        b2.Subscribe<Login>([token3](const Login&) { (void)token3; });
        b2.Subscribe<Logout>([token3](const Logout&) { (void)token3; });
        CHECK(token3.use_count() == 3);
        b2.Clear();
        CHECK(token3.use_count() == 1);
        b2.Subscribe<Login>([token3](const Login&) { (void)token3; });
    }
    CHECK(token3.use_count() == 1);
}

// 回调（的捕获物）析构时再来操作总线：退订、订阅新类型（会让内部的槽数组扩容）都要安全。
struct ReentrantDtor
{
    EventBus* bus = nullptr;
    SubId other = 0;
    int* destroyed = nullptr;
    ReentrantDtor(EventBus* b, SubId o, int* d)
        : bus(b)
        , other(o)
        , destroyed(d)
    {
    }
    ReentrantDtor(ReentrantDtor&& o) noexcept
        : bus(std::exchange(o.bus, nullptr))
        , other(o.other)
        , destroyed(o.destroyed)
    {
    }
    ReentrantDtor(const ReentrantDtor&) = delete;
    ~ReentrantDtor()
    {
        if (bus != nullptr)
        {
            bus->Unsubscribe(other);
            bus->Subscribe<Derived>([](const Derived&) {}); // 新类型：槽数组扩容
            ++*destroyed;
        }
    }
};

void TestHandlerDestructorMayTouchTheBus()
{
    EventBus bus;
    int destroyed = 0;
    int survivor_calls = 0;
    const SubId survivor = bus.Subscribe<Logout>([&](const Logout&) { ++survivor_calls; });
    const SubId victim =
        bus.Subscribe<Login>([guard = ReentrantDtor(&bus, survivor, &destroyed)](const Login&) { (void)guard; });
    CHECK(bus.Unsubscribe(victim)); // 销毁回调时它的析构退订了 survivor
    CHECK(destroyed == 1);
    bus.Publish(Logout{});
    CHECK(survivor_calls == 0);

    // Clear 里同样
    EventBus b2;
    int d2 = 0;
    const SubId s2 = b2.Subscribe<Logout>([](const Logout&) {});
    b2.Subscribe<Login>([guard = ReentrantDtor(&b2, s2, &d2)](const Login&) { (void)guard; });
    b2.Clear();
    CHECK(d2 == 1);
}

void TestPublishDepthLimit()
{
    QuietStderr quiet;
    EventBus bus;
    int depth = 0, max_depth = 0, calls = 0;
    bus.Subscribe<Login>(
        [&](const Login& e)
        {
            ++calls;
            ++depth;
            max_depth = std::max(max_depth, depth);
            bus.Publish(e); // 无限递归
            --depth;
        });
    // Release 路径：超限时丢弃这次发布并记日志（这里没有默认 logger，日志写到 stderr 兜底），不崩
    bus.Publish(Login{});
    CHECK(max_depth == EventBus::kMaxPublishDepth && calls == EventBus::kMaxPublishDepth);
    // 之后总线仍然可用、深度已经回到 0
    int n = 0;
    EventBus b2;
    b2.Subscribe<Logout>([&](const Logout&) { ++n; });
    b2.Publish(Logout{});
    CHECK(n == 1);
    calls = 0;
    depth = 0;
    max_depth = 0;
    bus.Publish(Login{});
    CHECK(calls == EventBus::kMaxPublishDepth);

    // 跨类型的环：A 的回调发布 B，B 的回调又发布 A
    EventBus c;
    int a_calls = 0, b_calls = 0;
    c.Subscribe<Login>(
        [&](const Login&)
        {
            ++a_calls;
            c.Publish(Logout{});
        });
    c.Subscribe<Logout>(
        [&](const Logout&)
        {
            ++b_calls;
            c.Publish(Login{});
        });
    c.Publish(Login{});
    CHECK(a_calls + b_calls == EventBus::kMaxPublishDepth);
}

// 回调抛异常：总线保持一致（深度与"正在发布"计数复位），异常传给调用方，之后照常工作；已取消的条目也会被清理。
void TestHandlerThrows()
{
    EventBus bus;
    std::string order;
    SubId victim = 0;
    bus.Subscribe<Login>([&](const Login&) { order += 'a'; });
    bus.Subscribe<Login>(
        [&](const Login&)
        {
            order += 'b';
            bus.Unsubscribe(victim); // 退订一个（延迟清理的条目），再抛
            throw std::runtime_error("boom");
        });
    victim = bus.Subscribe<Login>([&](const Login&) { order += 'c'; });
    bus.Subscribe<Login>([&](const Login&) { order += 'd'; });

    // 连抛超过嵌套深度上限那么多次：如果深度没有复位，第 9 次开始就会被当成递归丢弃
    for (int i = 0; i < EventBus::kMaxPublishDepth * 3; ++i)
    {
        order.clear();
        bool threw = false;
        try
        {
            bus.Publish(Login{});
        }
        catch (const std::runtime_error&)
        {
            threw = true;
        }
        CHECK(threw);
        CHECK(order == (i == 0 ? "ab" : "ab")); // 抛出之后的订阅者（d）本次不再调用
    }
    CHECK(bus.SubscriberCount<Login>() == 3); // victim 被清掉了，其余在
    EventBus ok;
    int n = 0;
    ok.Subscribe<Login>([&](const Login&) { ++n; });
    ok.Publish(Login{});
    CHECK(n == 1);
}

void TestScopedSubscription()
{
    EventBus bus;
    int n = 0;
    {
        auto guard = bus.SubscribeScoped<Login>([&](const Login&) { ++n; });
        CHECK(guard && guard.id() != kInvalidSubId);
        bus.Publish(Login{});
        CHECK(n == 1 && bus.size() == 1);
    }
    CHECK(bus.empty());
    bus.Publish(Login{});
    CHECK(n == 1);

    // 移动：只有最终持有者退订
    auto a = bus.SubscribeScoped<Login>([&](const Login&) { ++n; });
    ScopedSubscription b = std::move(a);
    CHECK(!a && b); // NOLINT
    ScopedSubscription c;
    c = std::move(b);
    CHECK(!b && c && bus.size() == 1); // NOLINT
    // 赋值会先退订被覆盖的
    auto d = bus.SubscribeScoped<Login>([&](const Login&) { ++n; });
    CHECK(bus.size() == 2);
    d = std::move(c);
    CHECK(bus.size() == 1);
    d.Reset();
    CHECK(bus.empty() && !d);
    d.Reset(); // 再 Reset：什么也不做

    // Release：放弃管理，订阅留着
    auto e = bus.SubscribeScoped<Login>([&](const Login&) { ++n; });
    const SubId id = e.Release();
    CHECK(!e && bus.size() == 1);
    CHECK(bus.Unsubscribe(id));

    // 守卫在回调里销毁自己所属的订阅
    EventBus b2;
    std::unique_ptr<ScopedSubscription> self;
    int calls = 0;
    self = std::make_unique<ScopedSubscription>(b2.SubscribeScoped<Login>(
        [&](const Login&)
        {
            ++calls;
            self.reset(); // 退订自己
        }));
    b2.Publish(Login{});
    b2.Publish(Login{});
    CHECK(calls == 1 && b2.empty());
}

// ---------------- 与朴素参考实现的随机对照 ----------------
//
// 参考实现用"快照"语义：Publish 时先拷贝一份 (订阅者指针) 列表，逐个检查 alive 再调用；Unsubscribe 置 alive=false
// 并从列表里删掉；Clear 把所有人置 alive=false。回调里按 (订阅者编号, 第几次被调用) 决定的脚本做操作
// （订阅新的 / 退订别人 / Clear / 嵌套发布），两边执行同一份脚本，调用序列必须完全一致。

struct EvA
{
    int tag = 0;
};
struct EvB
{
    int tag = 0;
};
struct EvC
{
    int tag = 0;
};

std::uint64_t Mix(std::uint64_t x)
{
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

struct RefBus
{
    struct Sub
    {
        int label;
        int type;
        bool alive = true;
        std::function<void()> fn;
    };
    std::vector<std::shared_ptr<Sub>> subs[3];
    int depth = 0;
};

constexpr int kMaxLabels = 400;

template <typename Real>
struct Harness;

// 真实总线的世界
struct RealWorld
{
    EventBus bus;
    std::vector<SubId> ids; // 标签 -> 句柄
    std::vector<int> calls; // 每个标签被调用的次数
    std::vector<int> trace;
    int depth = 0;

    int NewLabel()
    {
        ids.push_back(0);
        calls.push_back(0);
        return static_cast<int>(ids.size()) - 1;
    }
    template <typename E>
    void SubscribeLabel(int label)
    {
        ids[static_cast<std::size_t>(label)] = bus.Subscribe<E>([this, label](const E&) { OnCall(label); });
    }
    void SubscribeTyped(int type, int label)
    {
        if (type == 0)
            SubscribeLabel<EvA>(label);
        else if (type == 1)
            SubscribeLabel<EvB>(label);
        else
            SubscribeLabel<EvC>(label);
    }
    void PublishTyped(int type)
    {
        if (type == 0)
            bus.Publish(EvA{});
        else if (type == 1)
            bus.Publish(EvB{});
        else
            bus.Publish(EvC{});
    }
    void Unsub(int label) { bus.Unsubscribe(ids[static_cast<std::size_t>(label)]); }
    void ClearAll() { bus.Clear(); }
    std::size_t Size() { return bus.size(); }

    void OnCall(int label);
};

struct RefWorld
{
    RefBus bus;
    std::vector<std::shared_ptr<RefBus::Sub>> by_label;
    std::vector<int> calls;
    std::vector<int> trace;

    int NewLabel()
    {
        by_label.push_back(nullptr);
        calls.push_back(0);
        return static_cast<int>(by_label.size()) - 1;
    }
    void SubscribeTyped(int type, int label)
    {
        auto s = std::make_shared<RefBus::Sub>();
        s->label = label;
        s->type = type;
        s->fn = [this, label] { OnCall(label); };
        by_label[static_cast<std::size_t>(label)] = s;
        bus.subs[type].push_back(s);
    }
    void PublishTyped(int type)
    {
        if (bus.subs[type].empty())
            return;
        if (bus.depth >= EventBus::kMaxPublishDepth)
            return; // 与真实总线一致：超限丢弃
        ++bus.depth;
        const auto snapshot = bus.subs[type]; // 快照
        for (const auto& s : snapshot)
        {
            if (s->alive)
                s->fn();
        }
        --bus.depth;
    }
    void Unsub(int label)
    {
        auto& s = by_label[static_cast<std::size_t>(label)];
        if (!s || !s->alive)
            return;
        s->alive = false;
        auto& list = bus.subs[s->type];
        for (auto it = list.begin(); it != list.end(); ++it)
        {
            if (it->get() == s.get())
            {
                list.erase(it);
                break;
            }
        }
    }
    void ClearAll()
    {
        for (auto& list : bus.subs)
        {
            for (auto& s : list)
                s->alive = false;
            list.clear();
        }
    }
    std::size_t Size()
    {
        std::size_t n = 0;
        for (auto& list : bus.subs)
            n += list.size();
        return n;
    }

    void OnCall(int label);
};

template <typename W>
void RunScript(W& w, int label)
{
    const int k = w.calls[static_cast<std::size_t>(label)]++;
    w.trace.push_back(label);
    const std::uint64_t r = Mix(static_cast<std::uint64_t>(label) * 7919ULL + static_cast<std::uint64_t>(k));
    switch (r % 12)
    {
        case 0:
        case 1:
        {
            if (static_cast<int>(w.calls.size()) < kMaxLabels)
            {
                const int l = w.NewLabel();
                w.SubscribeTyped(static_cast<int>((r >> 8) % 3), l);
            }
            break;
        }
        case 2:
        case 3:
            w.Unsub(static_cast<int>((r >> 8) % w.calls.size()));
            break;
        case 4:
            w.Unsub(label); // 退订自己
            break;
        case 5:
            w.PublishTyped(static_cast<int>((r >> 8) % 3)); // 嵌套发布
            break;
        case 6:
            if ((r >> 8) % 6 == 0)
                w.ClearAll();
            break;
        default:
            break;
    }
}

void RealWorld::OnCall(int label) { RunScript(*this, label); }
void RefWorld::OnCall(int label) { RunScript(*this, label); }

void TestDifferential()
{
    QuietStderr quiet; // 脚本里的嵌套发布偶尔会超过深度上限，丢弃时断言库会记日志
    for (std::uint64_t seed = 1; seed <= 300; ++seed)
    {
        std::mt19937_64 rng(seed);
        RealWorld real;
        RefWorld ref;
        for (int step = 0; step < 150; ++step)
        {
            switch (rng() % 8)
            {
                case 0:
                case 1:
                case 2:
                {
                    if (static_cast<int>(real.calls.size()) < kMaxLabels)
                    {
                        const int type = static_cast<int>(rng() % 3);
                        const int l1 = real.NewLabel();
                        const int l2 = ref.NewLabel();
                        CHECK(l1 == l2);
                        real.SubscribeTyped(type, l1);
                        ref.SubscribeTyped(type, l2);
                    }
                    break;
                }
                case 3:
                case 4:
                {
                    if (!real.calls.empty())
                    {
                        const int l = static_cast<int>(rng() % real.calls.size());
                        real.Unsub(l);
                        ref.Unsub(l);
                    }
                    break;
                }
                case 5:
                case 6:
                {
                    const int type = static_cast<int>(rng() % 3);
                    real.PublishTyped(type);
                    ref.PublishTyped(type);
                    break;
                }
                default:
                    if (rng() % 10 == 0)
                    {
                        real.ClearAll();
                        ref.ClearAll();
                    }
                    break;
            }
            CHECK(real.trace == ref.trace);
            CHECK(real.Size() == ref.Size());
        }
        CHECK(real.calls == ref.calls);
    }
}

} // namespace

int main()
{
    g_real_stderr = ::dup(STDERR_FILENO);
    TestBasicDispatch();
    TestDispatchIsByStaticType();
    TestHandlerForms();
    TestUnsubscribe();
    TestSubscribeInsideHandlerDoesNotMoveRunningHandler();
    TestUnsubscribeInsideHandler();
    TestClearInsideHandler();
    TestNestedPublish();
    TestCancelledEntriesAreReleasedPromptly();
    TestHandlerDestructorMayTouchTheBus();
    TestPublishDepthLimit();
    TestHandlerThrows();
    TestScopedSubscription();
    TestDifferential();
    std::puts("event_bus_test: ok");
    return 0;
}
