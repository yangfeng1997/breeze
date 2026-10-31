// fsm::Machine：进入 / 离开 / 事件的调用顺序、可选的成员、转移与事件的延迟执行（run-to-completion）、错误与否决、
// 循环保护、Start 语义、异常、状态类型的要求，以及随机操作下的不变量。
// 状态的成员函数都定义在所有状态之后（见 fsm.h 开头的说明）。无测试框架依赖：失败即 abort。

#include "base/fsm/fsm.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <variant>
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

std::vector<std::string> g_log;
void Log(const std::string& s) { g_log.push_back(s); }
std::string Joined()
{
    std::string out;
    for (const auto& s : g_log)
        out += (out.empty() ? "" : ",") + s;
    return out;
}
void Reset() { g_log.clear(); }

// ======================================================================================================
// 1. 文件头里的例子：重试到 3 次就结束

namespace basic
{
struct Ctx
{
    int retries = 0;
};
struct EvStart
{
};
struct EvFail
{
};
struct EvDone
{
};
using Event = std::variant<EvStart, EvFail, EvDone>;
struct Idle;
struct Running;
struct Finished;
using Machine = fsm::Machine<Ctx, Event, Idle, Running, Finished>;
struct Idle
{
    int OnEvent(Machine& m, Ctx&, const EvStart&);
};
struct Running
{
    int OnEnter(Machine&, Ctx&);
    int OnEvent(Machine& m, Ctx&, const EvDone&);
    int OnEvent(Machine& m, Ctx&, const EvFail&);
};
struct Finished
{
};
int Idle::OnEvent(Machine& m, Ctx&, const EvStart&) { return m.Transition<Running>(); }
int Running::OnEnter(Machine&, Ctx& c)
{
    ++c.retries;
    return 0;
}
int Running::OnEvent(Machine& m, Ctx&, const EvDone&) { return m.Transition<Finished>(); }
int Running::OnEvent(Machine& m, Ctx& c, const EvFail&)
{
    return c.retries < 3 ? m.Transition<Running>() : m.Transition<Finished>();
}
} // namespace basic

void TestDocExample()
{
    using namespace basic;
    Machine m;
    CHECK(!m.started() && m.StateIndex() == Machine::npos && m.StateName().empty());
    CHECK(m.Start() == 0);
    CHECK(m.started() && m.Is<Idle>() && m.StateIndex() == 0);
    CHECK(m.OnEvent(EvStart{}) == 0 && m.Is<Running>() && m.Context().retries == 1);
    CHECK(m.OnEvent(EvFail{}) == 0 && m.Is<Running>() && m.Context().retries == 2); // 转移到自己：重新进入
    CHECK(m.OnEvent(EvFail{}) == 0 && m.Context().retries == 3 && m.Is<Running>());
    CHECK(m.OnEvent(EvFail{}) == 0 && m.Is<Finished>());
    CHECK(m.OnEvent(EvStart{}) == fsm::kUnhandled && m.Is<Finished>()); // Finished 什么也不处理
    CHECK(m.StateName().find("Finished") != std::string::npos && m.StateIndex() == 2);
}

// ======================================================================================================
// 2. 调用顺序；成员都是可选的；void / int 返回

namespace order
{
struct Ctx
{
};
struct Go
{
};
struct Next
{
};
struct Back
{
};
using Event = std::variant<Go, Next, Back>;
struct A;
struct B;
struct C;
using Machine = fsm::Machine<Ctx, Event, A, B, C>;
struct A
{
    void OnEnter(Machine&, Ctx&); // void 返回
    int OnExit(Machine&, Ctx&);
    int OnEvent(Machine& m, Ctx&, const Go&);
};
struct B
{
    int OnEnter(Machine&, Ctx&);
    // 没有 OnExit
    int OnEvent(Machine& m, Ctx&, const Next&);
};
struct C
{
    // 既没有 OnEnter 也没有 OnExit
    void OnEvent(Machine& m, Ctx&, const Back&); // void 返回
};
void A::OnEnter(Machine&, Ctx&) { Log("enter A"); }
int A::OnExit(Machine&, Ctx&)
{
    Log("exit A");
    return 0;
}
int A::OnEvent(Machine& m, Ctx&, const Go&)
{
    Log("A got Go");
    return m.Transition<B>();
}
int B::OnEnter(Machine&, Ctx&)
{
    Log("enter B");
    return 0;
}
int B::OnEvent(Machine& m, Ctx&, const Next&)
{
    Log("B got Next");
    return m.Transition<C>();
}
void C::OnEvent(Machine& m, Ctx&, const Back&)
{
    Log("C got Back");
    m.Transition<A>();
}
} // namespace order

void TestCallOrderAndOptionalMembers()
{
    using namespace order;
    Reset();
    Machine m;
    CHECK(m.Start() == 0);
    CHECK(Joined() == "enter A");
    CHECK(m.OnEvent(Go{}) == 0);
    CHECK(Joined() == "enter A,A got Go,exit A,enter B");
    CHECK(m.OnEvent(Next{}) == 0 && m.Is<C>());
    CHECK(m.OnEvent(Back{}) == 0 && m.Is<A>());
    CHECK(Joined() == "enter A,A got Go,exit A,enter B,B got Next,C got Back,enter A");
    CHECK(m.OnEvent(Next{}) == fsm::kUnhandled && m.Is<A>()); // 没处理的事件
    CHECK(m.OnEvent(Back{}) == fsm::kUnhandled);
}

// ======================================================================================================
// 3. 转移是延迟的：回调里请求转移之后还能用自己的成员；同一回调里多次请求以最后一次为准；出错就丢弃

namespace deferred
{
struct Ctx
{
    std::string seen;
};
struct Go
{
};
struct Fail
{
};
struct Twice
{
};
using Event = std::variant<Go, Fail, Twice>;
struct A;
struct B;
struct C;
using Machine = fsm::Machine<Ctx, Event, A, B, C>;
struct A
{
    std::string payload = std::string(100, 'p'); // 超出 SSO：状态对象被提前销毁的话，asan 会报
    int OnEvent(Machine& m, Ctx& c, const Go&);
    int OnEvent(Machine& m, Ctx&, const Fail&);
    int OnEvent(Machine& m, Ctx&, const Twice&);
};
struct B
{
};
struct C
{
};
int A::OnEvent(Machine& m, Ctx& c, const Go&)
{
    const int rc = m.Transition<B>();
    // 转移还没发生：自己还活着，成员可读可写（旧版在 Transition 里就把自己销毁了）
    payload += "x";
    c.seen = payload;
    CHECK(m.Is<A>());
    return rc;
}
int A::OnEvent(Machine& m, Ctx&, const Fail&)
{
    m.Transition<B>(); // 请求了转移又返回错误：转移丢弃
    return -7;
}
int A::OnEvent(Machine& m, Ctx&, const Twice&)
{
    m.Transition<B>();
    m.Transition<C>(); // 以最后一次为准
    return 0;
}
} // namespace deferred

void TestTransitionIsDeferred()
{
    using namespace deferred;
    {
        Machine m;
        m.Start();
        CHECK(m.OnEvent(Go{}) == 0 && m.Is<B>());
        CHECK(m.Context().seen.size() == 101 && m.Context().seen.back() == 'x');
    }
    {
        Machine m;
        m.Start();
        CHECK(m.OnEvent(Fail{}) == -7 && m.Is<A>()); // 错误：留在原状态，转移丢弃
    }
    {
        Machine m;
        m.Start();
        CHECK(m.OnEvent(Twice{}) == 0 && m.Is<C>());
    }
}

// ======================================================================================================
// 4. OnEnter 里的转移（链）、事件排队与 run-to-completion

namespace chain
{
struct Ctx
{
};
struct E1
{
};
struct E2
{
};
struct E3
{
};
using Event = std::variant<E1, E2, E3>;
struct A;
struct B;
struct C;
struct D;
using Machine = fsm::Machine<Ctx, Event, A, B, C, D>;
struct A
{
    int OnEnter(Machine& m, Ctx&);
    int OnExit(Machine&, Ctx&);
};
struct B
{
    int OnEnter(Machine& m, Ctx&);
    int OnExit(Machine&, Ctx&);
};
struct C
{
    int OnEnter(Machine& m, Ctx&);
    int OnEvent(Machine& m, Ctx&, const E1&);
    int OnEvent(Machine&, Ctx&, const E2&);
    int OnExit(Machine&, Ctx&);
};
struct D
{
    int OnEnter(Machine&, Ctx&);
    int OnEvent(Machine&, Ctx&, const E3&);
    int OnEvent(Machine&, Ctx&, const E2&);
};
int A::OnEnter(Machine& m, Ctx&)
{
    Log("enter A");
    return m.Transition<B>(); // 一进入就转到 B
}
int A::OnExit(Machine&, Ctx&)
{
    Log("exit A");
    return 0;
}
int B::OnEnter(Machine& m, Ctx&)
{
    Log("enter B");
    return m.Transition<C>();
}
int B::OnExit(Machine&, Ctx&)
{
    Log("exit B");
    return 0;
}
int C::OnEnter(Machine& m, Ctx&)
{
    Log("enter C");
    // 在 OnEnter 里排队两个事件：等 C 的进入完成后按顺序处理
    CHECK(m.busy());
    CHECK(m.OnEvent(E1{}) == 0);
    CHECK(m.OnEvent(E2{}) == 0);
    CHECK(m.queued_events() == 2);
    return 0;
}
int C::OnEvent(Machine& m, Ctx&, const E1&)
{
    Log("C got E1");
    m.OnEvent(E3{}); // 先排队一个事件、再请求转移：转移先做，E3 由新状态 D 处理
    return m.Transition<D>();
}
int C::OnEvent(Machine&, Ctx&, const E2&)
{
    Log("C got E2"); // 不会被调用：E1 引起了转移到 D，E2 轮到时当前状态已经是 D
    return 0;
}
int C::OnExit(Machine&, Ctx&)
{
    Log("exit C");
    return 0;
}
int D::OnEnter(Machine&, Ctx&)
{
    Log("enter D");
    return 0;
}
int D::OnEvent(Machine&, Ctx&, const E3&)
{
    Log("D got E3");
    return 0;
}
int D::OnEvent(Machine&, Ctx&, const E2&)
{
    Log("D got E2");
    return 0;
}
} // namespace chain

void TestChainedTransitionsAndQueuedEvents()
{
    using namespace chain;
    Reset();
    Machine m;
    CHECK(!m.busy());
    CHECK(m.Start() == 0);
    CHECK(!m.busy() && m.queued_events() == 0 && m.Is<D>());
    // A、B 的进入里各自转移；C 进入时排了 E1、E2；C 处理 E1 时又排了 E3 并请求转移到 D：
    // 转移先做（exit C, enter D），然后按先进先出处理剩下的 E2、E3，它们看到的是 D
    CHECK(Joined() == "enter A,exit A,enter B,exit B,enter C,C got E1,exit C,enter D,D got E2,D got E3");
}

// ======================================================================================================
// 5. 错误、否决、OnEnter 失败

namespace errors
{
struct Ctx
{
    int exit_result = 0;
    int enter_result = 0;
};
struct Go
{
};
struct Poke
{
};
using Event = std::variant<Go, Poke>;
struct A;
struct B;
using Machine = fsm::Machine<Ctx, Event, A, B>;
struct A
{
    int OnExit(Machine&, Ctx& c);
    int OnEvent(Machine& m, Ctx&, const Go&);
    int OnEvent(Machine&, Ctx&, const Poke&);
};
struct B
{
    int OnEnter(Machine& m, Ctx& c);
    int OnEvent(Machine&, Ctx&, const Poke&);
};
int A::OnExit(Machine&, Ctx& c)
{
    Log("exit A");
    return c.exit_result;
}
int A::OnEvent(Machine& m, Ctx&, const Go&)
{
    m.OnEvent(Poke{}); // 排队的事件：出错时要被丢弃
    return m.Transition<B>();
}
int A::OnEvent(Machine&, Ctx&, const Poke&)
{
    Log("A got Poke");
    return 0;
}
int B::OnEnter(Machine& m, Ctx& c)
{
    Log("enter B");
    m.OnEvent(Poke{});
    return c.enter_result;
}
int B::OnEvent(Machine&, Ctx&, const Poke&)
{
    Log("B got Poke");
    return 0;
}
} // namespace errors

void TestVetoAndEnterFailure()
{
    using namespace errors;
    {
        // OnExit 返回非 0 = 否决：留在 A，错误码返回，排队的事件丢弃
        Reset();
        Machine m(Ctx{-3, 0});
        m.Start();
        CHECK(m.OnEvent(Go{}) == -3);
        CHECK(m.Is<A>() && m.queued_events() == 0 && !m.busy());
        CHECK(Joined() == "exit A"); // 没有进入 B，排队的 Poke 也没被处理
        m.Context().exit_result = 0; // 机器还能用：放开否决再来
        CHECK(m.OnEvent(Go{}) == 0 && m.Is<B>());
    }
    {
        // OnEnter 返回非 0：B 已经进入（回滚不了），错误码返回，排队的事件丢弃
        Reset();
        Machine m(Ctx{0, -9});
        m.Start();
        CHECK(m.OnEvent(Go{}) == -9);
        CHECK(m.Is<B>() && m.queued_events() == 0 && !m.busy());
        CHECK(Joined() == "exit A,enter B");
        CHECK(m.OnEvent(Poke{}) == 0); // 仍可用
        CHECK(Joined() == "exit A,enter B,B got Poke");
    }
}

namespace evfail
{
struct Ctx
{
};
struct Fail
{
    int code;
};
struct Queue
{
};
struct Poke
{
};
using Event = std::variant<Fail, Queue, Poke>;
struct A;
struct B;
using Machine = fsm::Machine<Ctx, Event, A, B>;
struct A
{
    int OnEvent(Machine& m, Ctx&, const Fail& e);
    int OnEvent(Machine& m, Ctx&, const Queue&);
    int OnEvent(Machine&, Ctx&, const Poke&);
};
struct B
{
};
int A::OnEvent(Machine& m, Ctx&, const Fail& e)
{
    Log("A got Fail");
    m.OnEvent(Poke{}); // 与转移一起：返回错误后都要丢弃
    m.Transition<B>();
    return e.code;
}
int A::OnEvent(Machine& m, Ctx&, const Queue&)
{
    m.OnEvent(Fail{-5}); // 排队的事件出错：外层 OnEvent 返回这个错误，后面排的 Poke 丢弃
    m.OnEvent(Poke{});
    return 0;
}
int A::OnEvent(Machine&, Ctx&, const Poke&)
{
    Log("A got Poke");
    return 0;
}
} // namespace evfail

void TestEventHandlerErrors()
{
    using namespace evfail;
    {
        // 事件回调自己返回错误：错误码原样返回，它排的事件与请求的转移都丢弃
        Reset();
        Machine m;
        m.Start();
        CHECK(m.OnEvent(Fail{-7}) == -7);
        CHECK(m.Is<A>() && m.queued_events() == 0 && !m.busy());
        CHECK(Joined() == "A got Fail");
        CHECK(m.OnEvent(Poke{}) == 0); // 没有残留的转移在等着：仍在 A，Poke 只处理这一次
        CHECK(m.Is<A>());
        CHECK(Joined() == "A got Fail,A got Poke");
    }
    {
        Reset();
        Machine m;
        m.Start();
        CHECK(m.OnEvent(Queue{}) == -5);
        CHECK(m.Is<A>() && m.queued_events() == 0 && !m.busy());
        CHECK(Joined() == "A got Fail"); // 排在错误后面的 Poke 没有被处理，请求的转移也没有做
    }
}

namespace startfail
{
struct Ctx
{
};
struct Ev
{
};
using Event = std::variant<Ev>;
struct S;
using Machine = fsm::Machine<Ctx, Event, S>;
struct S
{
    int OnEnter(Machine&, Ctx&) { return -4; }
};
} // namespace startfail

void TestStartReportsEnterFailure()
{
    using namespace startfail;
    Machine m;
    CHECK(m.Start() == -4);
    CHECK(m.started() && m.Is<S>()); // 已经进入了
}

// ======================================================================================================
// 6. 循环保护

namespace loop
{
struct Ctx
{
};
struct Spin
{
};
using Event = std::variant<Spin>;
struct A;
struct B;
using Machine = fsm::Machine<Ctx, Event, A, B>;
struct A
{
    int OnEnter(Machine& m, Ctx&);
};
struct B
{
    int OnEnter(Machine& m, Ctx&);
};
int A::OnEnter(Machine& m, Ctx&) { return m.Transition<B>(); }
int B::OnEnter(Machine& m, Ctx&) { return m.Transition<A>(); }
} // namespace loop

void TestInfiniteTransitionLoopIsCut()
{
    using namespace loop;
    Machine m;
    CHECK(m.Start() == fsm::kTooManySteps); // A 与 B 互相一进入就转移
    CHECK(!m.busy() && m.queued_events() == 0 && m.started());
    CHECK(m.Is<A>() || m.Is<B>()); // 状态机仍然一致，停在某个状态上
}

namespace spin
{
struct Ctx
{
};
struct Spin
{
};
using Event = std::variant<Spin>;
struct S;
using Machine = fsm::Machine<Ctx, Event, S>;
struct S
{
    int OnEvent(Machine& m, Ctx&, const Spin&) { return m.OnEvent(Spin{}); } // 事件一直排队自己
};
} // namespace spin

void TestEventSelfQueueLoopIsCut()
{
    using namespace spin;
    Machine m;
    m.Start();
    CHECK(m.OnEvent(Spin{}) == fsm::kTooManySteps);
    CHECK(!m.busy() && m.queued_events() == 0);
    CHECK(m.OnEvent(Spin{}) == fsm::kTooManySteps); // 之后照样可用（照样被截断）
}

// ======================================================================================================
// 7. 自转移、带参数的转移

namespace selfmove
{
struct Ctx
{
    int enters = 0;
};
struct Again
{
};
struct Probe
{
};
struct ToCounter
{
};
using Event = std::variant<Again, Probe, ToCounter>;
struct Plain;
struct Counter;
using Machine = fsm::Machine<Ctx, Event, Plain, Counter>;
struct Plain
{
    int data = 0;
    int OnEnter(Machine&, Ctx& c);
    int OnExit(Machine&, Ctx&);
    int OnEvent(Machine& m, Ctx&, const Again&);
    int OnEvent(Machine&, Ctx&, const Probe&);
    int OnEvent(Machine& m, Ctx&, const ToCounter&);
};
struct Counter
{
    int n;
    std::string name;
    Counter(int n_, std::string name_)
        : n(n_)
        , name(std::move(name_))
    {
    }
    int OnEvent(Machine&, Ctx&, const Probe&);
};
int Plain::OnEnter(Machine&, Ctx& c)
{
    ++c.enters;
    Log("enter Plain");
    return 0;
}
int Plain::OnExit(Machine&, Ctx&)
{
    Log("exit Plain");
    return 0;
}
int Plain::OnEvent(Machine& m, Ctx&, const Again&)
{
    data = 99;
    return m.Transition<Plain>(); // 转移到自己：完整的一次转移，状态对象重新构造
}
int Plain::OnEvent(Machine&, Ctx&, const Probe&)
{
    Log("data=" + std::to_string(data));
    return 0;
}
int Plain::OnEvent(Machine& m, Ctx&, const ToCounter&) { return m.Transition<Counter>(5, std::string("five")); }
int Counter::OnEvent(Machine&, Ctx&, const Probe&)
{
    Log(name + "=" + std::to_string(n));
    return 0;
}
} // namespace selfmove

void TestSelfTransitionAndTransitionArguments()
{
    using namespace selfmove;
    Reset();
    Machine m;
    m.Start();
    m.OnEvent(Again{});
    CHECK(Joined() == "enter Plain,exit Plain,enter Plain");
    CHECK(m.Context().enters == 2);
    // 状态对象被重新构造：回调里写的 data = 99 随旧对象一起销毁，新对象是 0
    Reset();
    m.OnEvent(Probe{});
    CHECK(Joined() == "data=0");
    // 用参数构造新状态
    Reset();
    m.OnEvent(ToCounter{});
    CHECK(m.Is<Counter>());
    m.OnEvent(Probe{});
    CHECK(Joined() == "exit Plain,five=5");
}

// ======================================================================================================
// 8. Start 的各种情形、外面发起的转移、状态类型的要求

namespace startup
{
struct Ctx
{
    int& counter; // 没有默认构造：必须显式传入 Ctx
};
struct Ev
{
};
using Event = std::variant<Ev>;
struct First;
struct Second;
using Machine = fsm::Machine<Ctx, Event, First, Second>;
struct First
{
    explicit First(int v)
        : value(v)
    {
    } // 没有默认构造
    int value;
    int OnEnter(Machine&, Ctx& c)
    {
        c.counter += 1;
        return 0;
    }
};
struct Second
{
    std::unique_ptr<int> owned = std::make_unique<int>(7); // 只能移动
    const int fixed = 3;                                   // 有 const 成员：不可移动赋值，只要求可移动构造
    int OnEnter(Machine& m, Ctx& c);
};
int Second::OnEnter(Machine& m, Ctx& c)
{
    c.counter += 10;
    return m.Transition<First>(5); // 一进入就转到 First，带参数
}
} // namespace startup

void TestStartVariants()
{
    using namespace startup;
    int counter = 0;
    Machine m{Ctx{counter}};
    // Start 之前：什么都不能做
    CHECK(!m.started());
    CHECK(m.OnEvent(Ev{}) == fsm::kNotStarted);
    CHECK(m.Transition<Second>() == fsm::kNotStarted);
    CHECK(!m.Is<First>() && m.StateIndex() == Machine::npos);

    CHECK(m.Start<First>(3) == 0); // 第一个状态没有默认构造：带参数 Start
    CHECK(m.Is<First>() && counter == 1);
    CHECK(m.Start<First>(3) == fsm::kAlreadyStarted); // 不能重复 Start
    CHECK(counter == 1);

    // 在外面发起的转移：立刻执行完，并且按 OnEnter 的链继续
    CHECK(m.Transition<Second>() == 0);
    CHECK(m.Is<First>() && counter == 1 + 10 + 1); // Second 进入（+10）→ 转到 First(5)（+1）
}

void TestStartFromNonFirstState()
{
    using namespace startup;
    int counter = 0;
    Machine m{Ctx{counter}};
    CHECK(m.Start<Second>() == 0); // 从 Second 开始：进入 Second（+10），它转到 First(5)（+1）
    CHECK(m.Is<First>() && counter == 11);
}

void TestMachineIsNotCopyable()
{
    static_assert(!std::is_copy_constructible_v<startup::Machine> && !std::is_copy_assignable_v<startup::Machine>);
    static_assert(!std::is_copy_constructible_v<basic::Machine>);
}

// ======================================================================================================
// 9. 回调抛异常

namespace thrower
{
struct Ctx
{
};
struct Boom
{
};
struct Chain
{
};
struct Ok
{
};
using Event = std::variant<Boom, Chain, Ok>;
struct A;
struct B;
using Machine = fsm::Machine<Ctx, Event, A, B>;
struct A
{
    int OnEvent(Machine& m, Ctx&, const Boom&);
    int OnEvent(Machine& m, Ctx&, const Chain&);
    int OnEvent(Machine&, Ctx&, const Ok&);
};
struct B
{
    int OnEnter(Machine&, Ctx&) { throw std::runtime_error("enter boom"); }
};
int A::OnEvent(Machine& m, Ctx&, const Boom&)
{
    m.OnEvent(Ok{});   // 排了一个事件
    m.Transition<B>(); // 登记了一个转移
    throw std::runtime_error("boom");
}
int A::OnEvent(Machine& m, Ctx&, const Chain&) { return m.Transition<B>(); }
int A::OnEvent(Machine&, Ctx&, const Ok&)
{
    Log("A got Ok");
    return 0;
}
} // namespace thrower

void TestHandlerThrows()
{
    using namespace thrower;
    Reset();
    Machine m;
    m.Start();
    bool threw = false;
    try
    {
        m.OnEvent(Boom{});
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    CHECK(threw);
    // 状态机保持可用：不再"正在分发"，排队的事件与登记的转移都丢了
    CHECK(!m.busy() && m.queued_events() == 0 && m.Is<A>());
    CHECK(Joined().empty());
    CHECK(m.OnEvent(Ok{}) == 0 && Joined() == "A got Ok");

    // OnEnter 抛：旧状态已经被替换，新状态在场；状态机仍然可用
    threw = false;
    try
    {
        m.OnEvent(Chain{});
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    CHECK(threw && !m.busy() && m.Is<B>());
    CHECK(m.OnEvent(Ok{}) == fsm::kUnhandled); // B 不处理 Ok
}

// ======================================================================================================
// 10. 随机操作下的不变量

namespace randomized
{
constexpr int kStates = 4;
struct Ctx
{
    std::mt19937_64 rng{1};
    long enters = 0;
    long exits = 0;
    int last_entered = -1;
};
struct Ea
{
};
struct Eb
{
};
struct Ec
{
};
using Event = std::variant<Ea, Eb, Ec>;
template <int I>
struct St;
using Machine = fsm::Machine<Ctx, Event, St<0>, St<1>, St<2>, St<3>>;

constexpr std::uint32_t kMagic = 0xA11CE5;

template <int I>
struct St
{
    std::uint32_t magic = kMagic;
    St() = default;
    St(St&& o) noexcept
        : magic(o.magic)
    {
        o.magic = 0;
    }
    ~St() { magic = 0; }

    // 请求转移到随机状态
    static void RandomTransition(Machine& m, Ctx& c)
    {
        switch (c.rng() % kStates)
        {
            case 0:
                m.Transition<St<0>>();
                break;
            case 1:
                m.Transition<St<1>>();
                break;
            case 2:
                m.Transition<St<2>>();
                break;
            default:
                m.Transition<St<3>>();
                break;
        }
    }
    static void RandomPost(Machine& m, Ctx& c)
    {
        switch (c.rng() % 3)
        {
            case 0:
                m.OnEvent(Ea{});
                break;
            case 1:
                m.OnEvent(Eb{});
                break;
            default:
                m.OnEvent(Ec{});
                break;
        }
    }
    int Act(Machine& m, Ctx& c)
    {
        CHECK(magic == kMagic); // 回调只会在活着的状态对象上调用
        CHECK(m.StateIndex() == static_cast<std::size_t>(I));
        switch (c.rng() % 10)
        {
            case 0:
            case 1:
            case 2:
                RandomTransition(m, c);
                return 0;
            case 3:
                RandomPost(m, c);
                return 0;
            case 4:
                RandomPost(m, c);
                RandomTransition(m, c);
                return 0;
            case 5:
                RandomTransition(m, c);
                return -1; // 出错：登记的转移与排队的事件都要被丢弃
            case 6:
                return fsm::kUnhandled;
            case 7:
                RandomTransition(m, c);
                RandomTransition(m, c);
                return 0;
            default:
                return 0;
        }
    }
    int OnEnter(Machine& m, Ctx& c)
    {
        CHECK(magic == kMagic);
        ++c.enters;
        c.last_entered = I;
        switch (c.rng() % 12)
        {
            case 0:
            case 1:
                RandomTransition(m, c);
                return 0;
            case 2:
                RandomPost(m, c);
                return 0;
            case 3:
                return -2; // 进入失败
            default:
                return 0;
        }
    }
    int OnExit(Machine&, Ctx& c)
    {
        CHECK(magic == kMagic);
        if (c.rng() % 20 == 0)
            return -3; // 否决
        ++c.exits;
        return 0;
    }
    int OnEvent(Machine& m, Ctx& c, const Ea&) { return Act(m, c); }
    int OnEvent(Machine& m, Ctx& c, const Eb&) { return Act(m, c); }
    int OnEvent(Machine& m, Ctx& c, const Ec&) { return Act(m, c); }
};

void CheckInvariants(const Machine& m)
{
    const Ctx& c = m.Context();
    CHECK(!m.busy() && m.queued_events() == 0);                // 分发结束后一定是空闲的
    CHECK(c.enters - c.exits == 1);                            // 任何时刻恰好有一个"已进入、未离开"的状态
    CHECK(static_cast<int>(m.StateIndex()) == c.last_entered); // 当前状态就是最近进入的那个
}
} // namespace randomized

void TestRandomizedInvariants()
{
    using namespace randomized;
    for (std::uint64_t seed = 1; seed <= 40; ++seed)
    {
        Machine m;
        m.Context().rng.seed(seed);
        const int start_rc = m.Start();
        CHECK(start_rc == 0 || start_rc == -2 || start_rc == fsm::kTooManySteps || start_rc == -3 || start_rc == -1);
        CheckInvariants(m);
        std::mt19937_64 driver(seed * 977);
        long ok = 0, errors = 0, unhandled = 0;
        for (int i = 0; i < 4000; ++i)
        {
            int rc = 0;
            switch (driver() % 5)
            {
                case 0:
                    rc = m.OnEvent(Ea{});
                    break;
                case 1:
                    rc = m.OnEvent(Eb{});
                    break;
                case 2:
                    rc = m.OnEvent(Ec{});
                    break;
                case 3:
                    rc = m.Transition<St<0>>();
                    break;
                default:
                    rc = m.Transition<St<2>>();
                    break;
            }
            CheckInvariants(m);
            if (rc == 0)
                ++ok;
            else if (rc == fsm::kUnhandled)
                ++unhandled;
            else
                ++errors;
        }
        CHECK(ok > 300 && errors > 100 && unhandled > 100); // 三种结局都出现过，用例才有意义
    }
}

} // namespace

int main()
{
    TestDocExample();
    TestCallOrderAndOptionalMembers();
    TestTransitionIsDeferred();
    TestChainedTransitionsAndQueuedEvents();
    TestVetoAndEnterFailure();
    TestEventHandlerErrors();
    TestStartReportsEnterFailure();
    TestInfiniteTransitionLoopIsCut();
    TestEventSelfQueueLoopIsCut();
    TestSelfTransitionAndTransitionArguments();
    TestStartVariants();
    TestStartFromNonFirstState();
    TestMachineIsNotCopyable();
    TestHandlerThrows();
    TestRandomizedInvariants();
    std::puts("fsm_test: ok");
    return 0;
}
