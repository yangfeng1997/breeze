#pragma once

// 两段式 trampoline。
//
// 为什么不是“catch 之后直接 lua_error”：
//   Lua 按 C 编译时 lua_error 是 longjmp，会跳过栈上所有 C++ 析构。
//   所以这里把工作拆成两层：
//     Trampoline  只有平凡局部变量（ErrorSlot / int / 引用），必要时在这里 lua_error；
//     Invoke      [[gnu::noinline]]，所有参数、返回值、用户函数都在这一层，
//                 它正常返回之后 Trampoline 才决定是否报错。
//   用户函数如果抛异常（仅当编译时开了异常），在 Invoke 里捕获并转成 ErrorSlot，
//   异常对象在离开 catch 前析构，不会穿过 Lua 的 C 帧。

#include <cstddef>
#include <exception>
#include <functional>
#include <tuple>
#include <type_traits>
#include <utility>

#include "base/fastlua/config.h"
#include "base/fastlua/error.h"
#include "base/fastlua/stack.h"

namespace breeze::fastlua {

template <class T>
inline constexpr bool kInjectedArg = std::is_same_v<std::remove_cvref_t<T>, lua_State*>;

template <class T>
using ArgStorage = std::remove_cvref_t<T>;

// ---------- callable traits ----------
template <class F>
struct function_traits;

template <class C, class R, class... A>
struct function_traits<R (C::*)(A...)> {
    using class_type = C;
    using return_type = R;
    using arg_tuple = std::tuple<C*, A...>;
    using method_args = std::tuple<A...>;
    static constexpr bool kMember = true;
};
template <class C, class R, class... A>
struct function_traits<R (C::*)(A...) const> {
    using class_type = C;
    using return_type = R;
    using arg_tuple = std::tuple<C*, A...>;
    using method_args = std::tuple<A...>;
    static constexpr bool kMember = true;
};
template <class C, class R, class... A>
struct function_traits<R (C::*)(A...) noexcept> : function_traits<R (C::*)(A...)> {};
template <class C, class R, class... A>
struct function_traits<R (C::*)(A...) const noexcept> : function_traits<R (C::*)(A...) const> {};

template <class R, class... A>
struct function_traits<R (*)(A...)> {
    using return_type = R;
    using arg_tuple = std::tuple<A...>;
    static constexpr bool kMember = false;
};
template <class R, class... A>
struct function_traits<R (*)(A...) noexcept> : function_traits<R (*)(A...)> {};

// lambda / functor：去掉成员函数隐含的 this 参数。
template <class T>
struct lambda_traits;
template <class C, class R, class... A>
struct lambda_traits<R (C::*)(A...)> {
    using return_type = R;
    using arg_tuple = std::tuple<A...>;
    static constexpr bool kMember = false;
};
template <class C, class R, class... A>
struct lambda_traits<R (C::*)(A...) const> : lambda_traits<R (C::*)(A...)> {};
template <class C, class R, class... A>
struct lambda_traits<R (C::*)(A...) noexcept> : lambda_traits<R (C::*)(A...)> {};
template <class C, class R, class... A>
struct lambda_traits<R (C::*)(A...) const noexcept> : lambda_traits<R (C::*)(A...)> {};

template <class F>
struct function_traits : lambda_traits<decltype(&F::operator())> {
    static constexpr bool kMember = false;
};

// ---------- 参数下标 ----------
template <class Tuple, std::size_t... J>
constexpr int CountConsumed(std::index_sequence<J...>) {
    return (0 + ... + (kInjectedArg<std::tuple_element_t<J, Tuple>> ? 0 : 1));
}

template <class Tuple, std::size_t I>
inline constexpr int kArgIndex = 1 + CountConsumed<Tuple>(std::make_index_sequence<I>{});

// ---------- 检查与读取 ----------
template <class T>
bool CheckOne(lua_State* L, int idx, ErrorSlot& err) {
    if constexpr (kInjectedArg<T>) {
        (void)L;
        (void)idx;
        return true;
    } else {
        if (!Stack<ArgStorage<T>>::Check(L, idx)) {
            err.SetArg(idx, Stack<ArgStorage<T>>::kName);
            return false;
        }
        return true;
    }
}

template <class T>
ArgStorage<T> GetOne(lua_State* L, int idx) {
    if constexpr (kInjectedArg<T>) {
        (void)idx;
        return L;
    } else {
        return Stack<ArgStorage<T>>::Get(L, idx);
    }
}

template <class Tuple, std::size_t... I>
bool CheckArgs(lua_State* L, ErrorSlot& err, Tuple*, std::index_sequence<I...>) {
    bool ok = true;
    ((ok = ok && CheckOne<std::tuple_element_t<I, Tuple>>(L, kArgIndex<Tuple, I>, err)), ...);
    return ok;
}

template <class Tuple, std::size_t... I>
auto BuildArgs(lua_State* L, Tuple*, std::index_sequence<I...>) {
    using result_tuple = std::tuple<ArgStorage<std::tuple_element_t<I, Tuple>>...>;
    return result_tuple{GetOne<std::tuple_element_t<I, Tuple>>(L, kArgIndex<Tuple, I>)...};
}

// 带 offset：成员函数/构造函数在索引 1 处还有 self 或 class 表，参数从 offset+1 开始。
template <class Tuple, std::size_t... I>
bool CheckArgsAt(lua_State* L, ErrorSlot& err, int offset, Tuple*, std::index_sequence<I...>) {
    bool ok = true;
    ((ok = ok && CheckOne<std::tuple_element_t<I, Tuple>>(L, offset + kArgIndex<Tuple, I>, err)), ...);
    return ok;
}

template <class Tuple, std::size_t... I>
auto BuildArgsAt(lua_State* L, int offset, Tuple*, std::index_sequence<I...>) {
    using result_tuple = std::tuple<ArgStorage<std::tuple_element_t<I, Tuple>>...>;
    return result_tuple{GetOne<std::tuple_element_t<I, Tuple>>(L, offset + kArgIndex<Tuple, I>)...};
}

// ---------- 调用 ----------
template <class F>
[[gnu::noinline]] int Invoke(lua_State* L, F& fn, ErrorSlot& err) {
    using traits = function_traits<F>;
    static_assert(!traits::kMember,
                  "fastlua: member function binding is only available through the class builder (M3)");
    using args_tuple = typename traits::arg_tuple;
    constexpr std::size_t kArity = std::tuple_size_v<args_tuple>;

    if (!CheckArgs(L, err, static_cast<args_tuple*>(nullptr), std::make_index_sequence<kArity>{})) {
        return -1;
    }

    auto args = BuildArgs(L, static_cast<args_tuple*>(nullptr), std::make_index_sequence<kArity>{});
    using R = typename traits::return_type;

#if defined(__cpp_exceptions)
    try {
#endif
        if constexpr (std::is_void_v<R>) {
            std::apply(fn, args);
            return 0;
        } else {
            R result = std::apply(fn, args);
            return PushReturn(L, std::move(result));
        }
#if defined(__cpp_exceptions)
    } catch (const std::exception& e) {
        err.Set(ErrorCode::kRuntime, e.what());
        return -1;
    } catch (...) {
        err.Set(ErrorCode::kRuntime, "unknown C++ exception in a bound Lua function");
        return -1;
    }
#endif
}

// 只有平凡局部变量；Invoke 返回后才会 lua_error。
template <class F>
int Trampoline(lua_State* L, F& fn) {
    ErrorSlot err{};
    const int result = Invoke<F>(L, fn, err);
    if (result >= 0) {
        return result;
    }
    RaiseError(L, err);
}

// 编译期函数指针：零 upvalue。
template <auto F>
int TrampStatic(lua_State* L) {
    static auto fn = F;
    return Trampoline<decltype(F)>(L, fn);
}

// 运行期可调用对象：注册时放在 userdata upvalue 里，每次调用不分配。
struct alignas(std::max_align_t) CallableHeader {
    void (*destroy)(void*) noexcept;
};

template <class F>
void DestroyCallable(void* payload) noexcept {
    static_cast<F*>(payload)->~F();
}

template <class F>
int TrampCallable(lua_State* L) {
    auto* header = static_cast<CallableHeader*>(lua_touserdata(L, lua_upvalueindex(1)));
    FASTLUA_CHECK(header != nullptr && header->destroy != nullptr, "fastlua: callable upvalue is invalid");
    auto* fn = reinterpret_cast<F*>(header + 1);
    return Trampoline<F>(L, *fn);
}

inline int CallableGc(lua_State* L) {
    auto* header = static_cast<CallableHeader*>(lua_touserdata(L, 1));
    if (header != nullptr && header->destroy != nullptr) {
        header->destroy(header + 1);
        header->destroy = nullptr;
    }
    return 0;
}

}  // namespace breeze::fastlua
