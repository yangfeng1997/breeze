// 编译期的有限状态机：状态是一个个普通结构体，事件是一个 std::variant，转移写在状态的 OnEvent 里。
//
//   struct Ctx { int retries = 0; };
//   struct EvStart {};  struct EvFail {};  struct EvDone {};
//   using Event = std::variant<EvStart, EvFail, EvDone>;
//
//   struct Idle; struct Running; struct Finished;
//   using Machine = breeze::fsm::Machine<Ctx, Event, Idle, Running, Finished>;
//
//   struct Idle    { int OnEvent(Machine& m, Ctx&, const EvStart&); };
//   struct Running {
//       int OnEnter(Machine&, Ctx&);
//       int OnEvent(Machine& m, Ctx&, const EvDone&);
//       int OnEvent(Machine& m, Ctx&, const EvFail&);
//   };
//   struct Finished {};
//
//   // 成员函数的定义要放在**所有状态都定义完之后**（通常是同一个头文件的下半部分或 .cpp）：
//   // 函数体里调用 m.Transition<Running>() 需要状态机类型完整，状态机又要求它的全部状态是完整类型。
//   inline int Idle::OnEvent(Machine& m, Ctx&, const EvStart&) { return m.Transition<Running>(); }
//   inline int Running::OnEnter(Machine&, Ctx& c) { ++c.retries; return 0; }
//   inline int Running::OnEvent(Machine& m, Ctx&, const EvDone&) { return m.Transition<Finished>(); }
//   inline int Running::OnEvent(Machine& m, Ctx& c, const EvFail&)
//   {
//       return c.retries < 3 ? m.Transition<Running>() : m.Transition<Finished>();
//   }
//
//   Machine m;            // 或 Machine m(Ctx{...});
//   m.Start();            // 进入第一个状态（调它的 OnEnter），也可以 m.Start<Running>(参数...) 从别的状态开始
//   m.OnEvent(EvStart{}); // 分发事件
//
// 状态可以有这三种成员，**都是可选的**（缺哪个就当什么也不做）：
//   int|void OnEnter(Machine&, Ctx&)                    进入时
//   int|void OnExit(Machine&, Ctx&)                     离开时
//   int|void OnEvent(Machine&, Ctx&, const E&)          处理事件 E（可以为不同的 E 写多个重载）
// 返回 int：0 = 成功，非 0 = 错误码；返回 void 等于返回 0。
//
// 语义（run-to-completion，和 UML 状态机一致）：
//   - **转移与事件都是延迟执行的**：在 OnEnter / OnExit / OnEvent 里调用 Transition<S>() 不会马上换状态，只是登记；
//     当前这个回调返回之后，机器才依次：调旧状态的 OnExit → 切换 → 调新状态的 OnEnter。所以回调里请求转移之后
//     还可以接着用自己的成员（旧版是在回调执行期间就把自己销毁了，之后再碰成员是未定义行为）。
//     同一个回调里请求了多次，以最后一次为准。
//   - 在回调里调用 OnEvent(e) 同样只是排队：等当前回调以及它引起的转移都做完，再按先进先出的顺序处理，新事件看到的是
//     转移之后的状态。旧版注释写着"入队、可重入"，其实没有队列，事件是直接递归分发的。
//   - 状态机外面（不在任何回调里）调用 Transition<S>() / OnEvent(e) 立刻执行到全部做完才返回。
//   - 返回值：第一个非 0 的错误码（来自 OnEvent / OnExit / OnEnter），没有错误是 0。出错就停：排队的事件与
//     待执行的转移全部丢弃。
//       OnExit 返回非 0 = 否决这次转移，仍留在原状态。
//       OnEnter 返回非 0 = 新状态已经进入（OnEnter 已经开始执行，没法回滚），错误码返回给调用方。
//     最外层分发的那个事件当前状态没有处理它：返回 fsm::kUnhandled（它与普通错误码不会重合：旧版是 -1，和普通的 -1
//     混了）；排队的事件没人处理则默默忽略。
//   - 转移到当前状态也是一次完整的转移（OnExit，状态对象重新构造，OnEnter）。Transition<S>(参数...) 用参数构造新状态。
//   - 一次分发里转移 + 处理的排队事件总数超过 kMaxStepsPerDispatch（256）返回 fsm::kTooManySteps：
//     防状态间来回无限转移。
//   - 构造函数不调用任何状态的回调：要显式调 Start()（旧版在构造函数里调 OnEnter，那时外层对象往往还没构造完，
//     错误码也丢了）。Start 之前分发事件返回 kNotStarted。
//   - 回调抛异常：状态机保持可用（"正在分发"标记复位，排队的事件和待执行的转移丢弃）。
//   - 状态类型要可移动构造（待执行的转移先构造出新状态、再搬进去）。事件是 std::variant 里的类型，会被拷贝进队列。
//   - 不是线程安全的：只在一个线程里用。回调里不要销毁状态机自己。

#pragma once

#include <cxxabi.h>

#include <cstddef>
#include <cstdlib>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <variant>

namespace breeze::fsm
{

// 分发的特殊返回值：取 int 的最小几个值，不会和业务错误码（通常是 -1、-2…）重合。
inline constexpr int kUnhandled = std::numeric_limits<int>::min();      // 最外层事件在当前状态没有处理者
inline constexpr int kNotStarted = std::numeric_limits<int>::min() + 1; // 还没有 Start
inline constexpr int kAlreadyStarted = std::numeric_limits<int>::min() + 2;
inline constexpr int kTooManySteps = std::numeric_limits<int>::min() + 3; // 一次分发里转移 / 事件太多，疑似循环
inline constexpr int kMaxStepsPerDispatch = 256;

// 类型名（demangle 过的），给日志用。
template <typename T>
std::string TypeName()
{
    int status = 0;
    std::unique_ptr<char, decltype(&std::free)> name(abi::__cxa_demangle(typeid(T).name(), nullptr, nullptr, &status),
                                                     &std::free);
    return (status == 0 && name) ? std::string(name.get()) : std::string(typeid(T).name());
}

namespace detail
{

template <typename T, typename... Ts>
inline constexpr bool is_one_of_v = (std::is_same_v<T, Ts> || ...);

template <typename T>
struct IsVariant : std::false_type
{
};
template <typename... Ts>
struct IsVariant<std::variant<Ts...>> : std::true_type
{
};

template <typename S, typename M, typename C>
concept HasOnEnter = requires(S& s, M& m, C& c) { s.OnEnter(m, c); };
template <typename S, typename M, typename C>
concept HasOnExit = requires(S& s, M& m, C& c) { s.OnExit(m, c); };
template <typename S, typename M, typename C, typename E>
concept HasOnEvent = requires(S& s, M& m, C& c, const E& e) { s.OnEvent(m, c, e); };

// 调用回调并把返回值统一成 int：void 当 0。
template <typename F>
int CallToCode(F&& f)
{
    using R = std::invoke_result_t<F>;
    if constexpr (std::is_void_v<R>)
    {
        std::forward<F>(f)();
        return 0;
    }
    else
    {
        static_assert(std::is_convertible_v<R, int>, "OnEnter / OnExit / OnEvent 要返回 int 或 void");
        return static_cast<int>(std::forward<F>(f)());
    }
}

} // namespace detail

template <typename Ctx, typename Event, typename... States>
class Machine
{
    static_assert(sizeof...(States) > 0, "至少要有一个状态");
    static_assert(detail::IsVariant<Event>::value, "Event 必须是 std::variant<各个事件类型...>");
    using First = std::tuple_element_t<0, std::tuple<States...>>;

public:
    using ContextType = Ctx;
    using EventType = Event;
    using State = std::variant<States...>;

    explicit Machine(Ctx ctx = Ctx{})
        : ctx_(std::move(ctx))
    {
    }
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;

    // 进入初始状态（默认是 States 里的第一个）：调它的 OnEnter，并处理 OnEnter 里引起的转移与事件。返回值规则见文件头。
    template <typename Initial = First, typename... Args>
    int Start(Args&&... args)
    {
        static_assert(detail::is_one_of_v<Initial, States...>, "Initial 必须是状态机的状态之一");
        if (state_.has_value())
            return kAlreadyStarted;
        state_.emplace(std::in_place_type<Initial>, std::forward<Args>(args)...);
        BusyScope scope(*this);
        const int rc = EnterCurrent();
        if (rc != 0)
        {
            Abort();
            return rc;
        }
        return Drain();
    }

    // 分发事件。在回调里调用只是排队（返回 0）。
    int OnEvent(const Event& ev)
    {
        if (!state_.has_value())
            return kNotStarted;
        if (busy_)
        {
            queue_.push_back(ev);
            return 0;
        }
        BusyScope scope(*this);
        const int rc = HandleEvent(ev);
        if (rc != 0 && rc != kUnhandled)
        {
            Abort();
            return rc;
        }
        const int drained = Drain();
        if (drained != 0)
            return drained;
        return rc; // 0 或 kUnhandled
    }

    // 任何属于 Event 的事件类型都可以直接传：m.OnEvent(EvStart{}) 里 EvStart 隐式转成 Event。
    // 转移到状态 S（用 args 构造）。在回调里调用只是登记（返回 0）；在外面调用立刻执行完。
    template <typename S, typename... Args>
    int Transition(Args&&... args)
    {
        static_assert(detail::is_one_of_v<S, States...>, "S 必须是状态机的状态之一");
        if (!state_.has_value())
            return kNotStarted;
        pending_.emplace(std::in_place_type<S>, std::forward<Args>(args)...);
        if (busy_)
            return 0;
        BusyScope scope(*this);
        return Drain();
    }

    // ---- 查看 ----

    bool started() const noexcept { return state_.has_value(); }
    // 正在某个回调里（此时 Transition / OnEvent 只是排队）。
    bool busy() const noexcept { return busy_; }
    // 排队中的事件个数。
    std::size_t queued_events() const noexcept { return queue_.size(); }

    template <typename S>
    [[nodiscard]] bool Is() const noexcept
    {
        static_assert(detail::is_one_of_v<S, States...>, "S 必须是状态机的状态之一");
        return state_.has_value() && std::holds_alternative<S>(*state_);
    }
    // 当前状态在 States 里的下标；没 Start 返回 npos。
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);
    [[nodiscard]] std::size_t StateIndex() const noexcept { return state_.has_value() ? state_->index() : npos; }
    // 当前状态的类型名（demangle 过）；没 Start 是空串。
    [[nodiscard]] std::string StateName() const
    {
        if (!state_.has_value())
            return {};
        return std::visit([](const auto& st) { return TypeName<std::decay_t<decltype(st)>>(); }, *state_);
    }

    [[nodiscard]] Ctx& Context() noexcept { return ctx_; }
    [[nodiscard]] const Ctx& Context() const noexcept { return ctx_; }

private:
    // 标记"正在分发"；离开时复位，如果是因为异常离开的，把没做完的转移与排队事件丢掉。
    struct BusyScope
    {
        explicit BusyScope(Machine& m)
            : machine(m)
            , exceptions(std::uncaught_exceptions())
        {
            machine.busy_ = true;
        }
        ~BusyScope()
        {
            machine.busy_ = false;
            if (std::uncaught_exceptions() > exceptions)
                machine.Abort();
        }
        Machine& machine;
        int exceptions;
    };

    void Abort() noexcept
    {
        pending_.reset();
        queue_.clear();
    }

    int EnterCurrent()
    {
        return std::visit(
            [this](auto& st) -> int
            {
                using S = std::decay_t<decltype(st)>;
                if constexpr (detail::HasOnEnter<S, Machine, Ctx>)
                    return detail::CallToCode([&] { return st.OnEnter(*this, ctx_); });
                else
                    return 0;
            },
            *state_);
    }

    int ExitCurrent()
    {
        return std::visit(
            [this](auto& st) -> int
            {
                using S = std::decay_t<decltype(st)>;
                if constexpr (detail::HasOnExit<S, Machine, Ctx>)
                    return detail::CallToCode([&] { return st.OnExit(*this, ctx_); });
                else
                    return 0;
            },
            *state_);
    }

    int HandleEvent(const Event& ev)
    {
        return std::visit(
            [this](auto& st, const auto& e) -> int
            {
                using S = std::decay_t<decltype(st)>;
                using E = std::decay_t<decltype(e)>;
                if constexpr (detail::HasOnEvent<S, Machine, Ctx, E>)
                    return detail::CallToCode([&] { return st.OnEvent(*this, ctx_, e); });
                else
                    return kUnhandled;
            },
            *state_,
            ev);
    }

    // 把待执行的转移与排队的事件处理到没有为止。转移优先于排队的事件（事件看到的是转移之后的状态）。
    int Drain()
    {
        int steps = 0;
        for (;;)
        {
            if (pending_.has_value())
            {
                if (++steps > kMaxStepsPerDispatch)
                {
                    Abort();
                    return kTooManySteps;
                }
                State next = std::move(*pending_);
                pending_.reset();
                if (const int rc = ExitCurrent(); rc != 0)
                {
                    Abort(); // OnExit 否决：留在原状态
                    return rc;
                }
                // 只要求状态可移动构造：variant 的移动赋值还要求可移动赋值，这里按下标原地构造
                std::visit([this](auto& s) { state_->template emplace<std::decay_t<decltype(s)>>(std::move(s)); },
                           next);
                if (const int rc = EnterCurrent(); rc != 0)
                {
                    Abort();
                    return rc;
                }
                continue;
            }
            if (!queue_.empty())
            {
                if (++steps > kMaxStepsPerDispatch)
                {
                    Abort();
                    return kTooManySteps;
                }
                const Event ev = std::move(queue_.front());
                queue_.pop_front();
                const int rc = HandleEvent(ev);
                if (rc != 0 && rc != kUnhandled)
                {
                    Abort();
                    return rc;
                }
                continue;
            }
            return 0;
        }
    }

    std::optional<State> state_;
    std::optional<State> pending_;
    std::deque<Event> queue_;
    Ctx ctx_;
    bool busy_ = false;
};

} // namespace breeze::fsm
