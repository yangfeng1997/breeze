#pragma once

// C++ 调 Lua。
// 每次调用：push 一个 light C 错误处理函数 → push 函数 → push 参数 → lua_pcall → settop。
// 错误处理函数没有 upvalue，lua_pushcfunction 不分配；它只在出错时生成 traceback。

#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "base/lua_binding/config.h"
#include "base/lua_binding/error.h"
#include "base/lua_binding/stack.h"

namespace breeze::lua_binding {

inline int MessageHandler(lua_State* L) {
    const char* message = lua_tostring(L, 1);
    if (message != nullptr) {
        luaL_traceback(L, L, message, 1);
    } else {
        lua_pushliteral(L, "(error object is not a string)");
    }
    return 1;
}

template <class R>
constexpr int ReturnCount() {
    using D = std::remove_cvref_t<R>;
    if constexpr (std::is_void_v<D>) {
        return 0;
    } else if constexpr (is_tuple_like_v<D>) {
        return static_cast<int>(std::tuple_size_v<D>);
    } else {
        return 1;
    }
}

inline std::string TakeError(lua_State* L, int base);

template <class Tuple, std::size_t... I>
Tuple ReadTuple(lua_State* L, int first, std::index_sequence<I...>);

template <class D>
inline Error ReturnTypeError(lua_State* L, int index) {
    const std::string actual = luaL_typename(L, index);
    return Error{ErrorCode::kType,
                 "breeze::lua_binding: return value expects " + std::string(Stack<D>::kName) + ", got " + actual};
}

template <class Tuple, std::size_t... I>
inline std::optional<Error> CheckTupleReturns(lua_State* L, int first, std::index_sequence<I...>) {
    if constexpr (sizeof...(I) > 0) {
        const bool checks[] = {
            Stack<std::tuple_element_t<I, Tuple>>::Check(L, first + static_cast<int>(I))...};
        const char* const names[] = {
            Stack<std::tuple_element_t<I, Tuple>>::kName...};
        for (std::size_t i = 0; i < sizeof...(I); ++i) {
            if (!checks[i]) {
                const std::string actual = luaL_typename(L, first + static_cast<int>(i));
                return Error{ErrorCode::kType,
                             "breeze::lua_binding: return value #" + std::to_string(i + 1) + " expects " +
                                 names[i] + ", got " + actual};
            }
        }
    }
    return std::nullopt;
}

// 不保护调用后的读返回值：类型不匹配是致命错误（Fatal），没有 std::expected 可返回。
template <class R>
R ReadResultsUnprotected(lua_State* L, int first_result, int restore) {
    using D = std::remove_cvref_t<R>;
    if constexpr (std::is_void_v<D>) {
        lua_settop(L, restore);
        return;
    } else if constexpr (is_tuple_like_v<D>) {
        if (auto error = CheckTupleReturns<D>(L, first_result, std::make_index_sequence<std::tuple_size_v<D>>{})) {
            const std::string message = std::move(error->message);
            lua_settop(L, restore);
            Fatal(message);
        }
        D value = ReadTuple<D>(L, first_result, std::make_index_sequence<std::tuple_size_v<D>>{});
        lua_settop(L, restore);
        return value;
    } else {
        if (!Stack<D>::Check(L, first_result)) {
            const Error error = ReturnTypeError<D>(L, first_result);
            const std::string message = error.message;
            lua_settop(L, restore);
            Fatal(message);
        }
        D value = Stack<D>::Get(L, first_result);
        lua_settop(L, restore);
        return value;
    }
}

// pcall 成功后读返回值。first_result 是第一个返回值的栈下标（pcall 已把结果数调整到
// ReturnCount<R>），restore 是调用前的栈顶（结果与错误处理函数都会被清掉）。
// 返回值类型不匹配是 kType，不能静默变成 0 / false / 空串。
template <class R>
std::expected<R, Error> ReadResults(lua_State* L, int first_result, int restore) {
    using D = std::remove_cvref_t<R>;
    if constexpr (std::is_void_v<D>) {
        lua_settop(L, restore);
        return {};
    } else if constexpr (is_tuple_like_v<D>) {
        if (auto error = CheckTupleReturns<D>(L, first_result, std::make_index_sequence<std::tuple_size_v<D>>{})) {
            lua_settop(L, restore);
            return std::unexpected(std::move(*error));
        }
        D value = ReadTuple<D>(L, first_result, std::make_index_sequence<std::tuple_size_v<D>>{});
        lua_settop(L, restore);
        return value;
    } else {
        if (!Stack<D>::Check(L, first_result)) {
            Error error = ReturnTypeError<D>(L, first_result);
            lua_settop(L, restore);
            return std::unexpected(std::move(error));
        }
        D value = Stack<D>::Get(L, first_result);
        lua_settop(L, restore);
        return value;
    }
}

// 函数已经在栈上（func_index）。压 msgh、把 msgh 移到函数下面，再调用。
// 与 CallGlobal 的区别只是“函数从哪来”。
template <class R, class... Args>
std::expected<R, Error> CallPushed(lua_State* L, int func_index, Args&&... args) {
    const int base = lua_absindex(L, func_index);
    if (!lua_checkstack(L, static_cast<int>(sizeof...(Args)) + 5)) {
        lua_settop(L, base - 1);
        return Fail(ErrorCode::kStack, "breeze::lua_binding: Lua stack overflow while pushing arguments");
    }
    lua_pushcfunction(L, &MessageHandler);
    lua_insert(L, base);  // [.., msgh, function]

    PushArgs(L, std::forward<Args>(args)...);

    const int status = lua_pcall(L, static_cast<int>(sizeof...(Args)), ReturnCount<R>(), base);
    if (status != LUA_OK) {
        std::string message = TakeError(L, base - 1);
        return Fail(status == LUA_ERRMEM ? ErrorCode::kMemory : ErrorCode::kLuaError, std::move(message));
    }
    return ReadResults<R>(L, base + 1, base - 1);
}

// registry 引用版本：先压 msgh，再压函数，省掉 CallPushed 的 lua_insert。
template <class R, class... Args>
std::expected<R, Error> CallRef(lua_State* L, int registry_ref, Args&&... args) {
    const int base = lua_gettop(L);
    if (!lua_checkstack(L, static_cast<int>(sizeof...(Args)) + 4)) {
        lua_settop(L, base);
        return Fail(ErrorCode::kStack, "breeze::lua_binding: Lua stack overflow while pushing arguments");
    }
    lua_pushcfunction(L, &MessageHandler);  // base + 1
    lua_rawgeti(L, LUA_REGISTRYINDEX, registry_ref);  // base + 2

    PushArgs(L, std::forward<Args>(args)...);
    const int status = lua_pcall(L, static_cast<int>(sizeof...(Args)), ReturnCount<R>(), base + 1);
    if (status != LUA_OK) {
        std::string message = TakeError(L, base);
        return Fail(status == LUA_ERRMEM ? ErrorCode::kMemory : ErrorCode::kLuaError, std::move(message));
    }
    return ReadResults<R>(L, base + 2, base);
}

template <class R, class... Args>
std::expected<R, Error> CallRefNoTraceback(lua_State* L, int registry_ref, Args&&... args) {
    const int base = lua_gettop(L);
    if (!lua_checkstack(L, static_cast<int>(sizeof...(Args)) + 3)) {
        lua_settop(L, base);
        return Fail(ErrorCode::kStack, "breeze::lua_binding: Lua stack overflow while pushing arguments");
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, registry_ref);  // base + 1

    PushArgs(L, std::forward<Args>(args)...);
    const int status = lua_pcall(L, static_cast<int>(sizeof...(Args)), ReturnCount<R>(), 0);
    if (status != LUA_OK) {
        std::string message = TakeError(L, base);
        return Fail(status == LUA_ERRMEM ? ErrorCode::kMemory : ErrorCode::kLuaError, std::move(message));
    }
    if constexpr (std::is_void_v<std::remove_cvref_t<R>>) {
        // lua_call(nresults = 0) 已经把函数和参数弹掉，栈顶就是 base。
        return {};
    } else {
        return ReadResults<R>(L, base + 1, base);
    }
}

// 无保护调用：lua_call，不捕获错误。Lua 抛错会 longjmp 穿过当前 C++ 栈；
// 只有确认被调函数不会报错时才用（或由外层 Lua pcall 接住，但中间不能有非平凡析构）。
template <class R, class... Args>
R CallRefUnprotected(lua_State* L, int registry_ref, Args&&... args) {
    const int base = lua_gettop(L);
    BREEZE_LUA_CHECK(lua_checkstack(L, static_cast<int>(sizeof...(Args)) + 2),
                  "breeze::lua_binding: Lua stack overflow while pushing arguments");
    lua_rawgeti(L, LUA_REGISTRYINDEX, registry_ref);
    PushArgs(L, std::forward<Args>(args)...);
    lua_call(L, static_cast<int>(sizeof...(Args)), ReturnCount<R>());
    if constexpr (std::is_void_v<std::remove_cvref_t<R>>) {
        return;
    } else {
        return ReadResultsUnprotected<R>(L, base + 1, base);
    }
}

template <class R, class... Args>
R CallGlobalUnprotected(lua_State* L, std::string_view name, Args&&... args) {
    const int base = lua_gettop(L);
    BREEZE_LUA_CHECK(lua_checkstack(L, static_cast<int>(sizeof...(Args)) + 2),
                  "breeze::lua_binding: Lua stack overflow while pushing arguments");
    const std::string key(name);
    lua_getglobal(L, key.c_str());
    BREEZE_LUA_CHECK(lua_isfunction(L, -1), "breeze::lua_binding: global '{}' is not a function", key);
    PushArgs(L, std::forward<Args>(args)...);
    lua_call(L, static_cast<int>(sizeof...(Args)), ReturnCount<R>());
    if constexpr (std::is_void_v<std::remove_cvref_t<R>>) {
        lua_settop(L, base);
        return;
    } else {
        return ReadResultsUnprotected<R>(L, base + 1, base);
    }
}

inline std::string TakeError(lua_State* L, int base) {
    std::string message;
    if (const char* text = lua_tostring(L, -1); text != nullptr) {
        message = text;
    } else {
        message = "lua error (non-string object)";
    }
    lua_settop(L, base);
    return message;
}

template <class Tuple, std::size_t... I>
Tuple ReadTuple(lua_State* L, int first, std::index_sequence<I...>) {
    return Tuple{Stack<std::tuple_element_t<I, Tuple>>::Get(L, first + static_cast<int>(I))...};
}

template <class R, class... Args>
std::expected<R, Error> CallGlobal(lua_State* L, std::string_view name, Args&&... args) {
    const int base = lua_gettop(L);
    if (!lua_checkstack(L, static_cast<int>(sizeof...(Args)) + 5)) {
        lua_settop(L, base);
        return Fail(ErrorCode::kStack, "breeze::lua_binding: Lua stack overflow while pushing arguments");
    }
    lua_pushcfunction(L, &MessageHandler);
    const int msgh = base + 1;

    const std::string key(name);
    lua_getglobal(L, key.c_str());
    if (!lua_isfunction(L, -1)) {
        const std::string actual = luaL_typename(L, -1);
        lua_settop(L, base);
        return Fail(ErrorCode::kNotFound,
                     "breeze::lua_binding: global '" + key + "' is not a function (got " + actual + ")");
    }
    // 栈已经是 [msgh][function]，不需要 lua_insert；pcall 的 errfunc 用 msgh 下标。
    PushArgs(L, std::forward<Args>(args)...);

    const int status = lua_pcall(L, static_cast<int>(sizeof...(Args)), ReturnCount<R>(), msgh);
    if (status != LUA_OK) {
        std::string message = TakeError(L, base);
        return Fail(status == LUA_ERRMEM ? ErrorCode::kMemory : ErrorCode::kLuaError, std::move(message));
    }
    return ReadResults<R>(L, base + 2, base);
}

}  // namespace breeze::lua_binding
