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

#include "base/fastlua/config.h"
#include "base/fastlua/error.h"
#include "base/fastlua/stack.h"

namespace breeze::fastlua {

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
                 "fastlua: return value expects " + std::string(Stack<D>::kName) + ", got " + actual};
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
                             "fastlua: return value #" + std::to_string(i + 1) + " expects " +
                                 names[i] + ", got " + actual};
            }
        }
    }
    return std::nullopt;
}

// pcall 成功后读返回值。first_result 是第一个返回值的栈下标（pcall 已把结果数调整到
// ReturnCount<R>），restore 是调用前的栈顶（结果与错误处理函数都会被清掉）。
// 返回值类型不匹配是 kType，不能静默变成 0 / false / 空串。
template <class R>
Result<R> ReadResults(lua_State* L, int first_result, int restore) {
    using D = std::remove_cvref_t<R>;
    if constexpr (std::is_void_v<D>) {
        lua_settop(L, restore);
        return Ok();
    } else if constexpr (is_tuple_like_v<D>) {
        if (auto error = CheckTupleReturns<D>(L, first_result, std::make_index_sequence<std::tuple_size_v<D>>{})) {
            lua_settop(L, restore);
            return std::move(*error);
        }
        D value = ReadTuple<D>(L, first_result, std::make_index_sequence<std::tuple_size_v<D>>{});
        lua_settop(L, restore);
        return value;
    } else {
        if (!Stack<D>::Check(L, first_result)) {
            Error error = ReturnTypeError<D>(L, first_result);
            lua_settop(L, restore);
            return error;
        }
        D value = Stack<D>::Get(L, first_result);
        lua_settop(L, restore);
        return value;
    }
}

// 函数已经在栈上（func_index）。压 msgh、把 msgh 移到函数下面，再调用。
// 与 CallGlobal 的区别只是“函数从哪来”。
template <class R, class... Args>
Result<R> CallPushed(lua_State* L, int func_index, Args&&... args) {
    const int base = lua_absindex(L, func_index);
    if (!lua_checkstack(L, static_cast<int>(sizeof...(Args)) + 5)) {
        lua_settop(L, base - 1);
        return Error{ErrorCode::kStack, "fastlua: Lua stack overflow while pushing arguments"};
    }
    lua_pushcfunction(L, &MessageHandler);
    lua_insert(L, base);  // [.., msgh, function]

    PushArgs(L, std::forward<Args>(args)...);

    const int status = lua_pcall(L, static_cast<int>(sizeof...(Args)), ReturnCount<R>(), base);
    if (status != LUA_OK) {
        std::string message = TakeError(L, base - 1);
        return Error{status == LUA_ERRMEM ? ErrorCode::kMemory : ErrorCode::kRuntime, std::move(message)};
    }
    return ReadResults<R>(L, base + 1, base - 1);
}

// 快路径：仍然 pcall（不会把错误变成 panic），但不挂 traceback、不 lua_insert。
// 适合“错误由上层处理、热路径不关心 traceback”的调用。
template <class R, class... Args>
Result<R> CallPushedFast(lua_State* L, int func_index, Args&&... args) {
    const int base = lua_absindex(L, func_index);
    if (!lua_checkstack(L, static_cast<int>(sizeof...(Args)) + 4)) {
        lua_settop(L, base - 1);
        return Error{ErrorCode::kStack, "fastlua: Lua stack overflow while pushing arguments"};
    }
    PushArgs(L, std::forward<Args>(args)...);
    const int status = lua_pcall(L, static_cast<int>(sizeof...(Args)), ReturnCount<R>(), 0);
    if (status != LUA_OK) {
        std::string message = TakeError(L, base - 1);
        return Error{status == LUA_ERRMEM ? ErrorCode::kMemory : ErrorCode::kRuntime, std::move(message)};
    }
    return ReadResults<R>(L, base, base - 1);
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
Result<R> CallGlobal(lua_State* L, std::string_view name, Args&&... args) {
    const int base = lua_gettop(L);
    if (!lua_checkstack(L, static_cast<int>(sizeof...(Args)) + 5)) {
        lua_settop(L, base);
        return Error{ErrorCode::kStack, "fastlua: Lua stack overflow while pushing arguments"};
    }
    lua_pushcfunction(L, &MessageHandler);
    const int msgh = base + 1;

    const std::string key(name);
    lua_getglobal(L, key.c_str());
    if (!lua_isfunction(L, -1)) {
        const std::string actual = luaL_typename(L, -1);
        lua_settop(L, base);
        return Error{ErrorCode::kNotFound,
                     "fastlua: global '" + key + "' is not a function (got " + actual + ")"};
    }
    // 栈已经是 [msgh][function]，不需要 lua_insert；pcall 的 errfunc 用 msgh 下标。
    PushArgs(L, std::forward<Args>(args)...);

    const int status = lua_pcall(L, static_cast<int>(sizeof...(Args)), ReturnCount<R>(), msgh);
    if (status != LUA_OK) {
        std::string message = TakeError(L, base);
        return Error{status == LUA_ERRMEM ? ErrorCode::kMemory : ErrorCode::kRuntime, std::move(message)};
    }
    return ReadResults<R>(L, base + 2, base);
}

}  // namespace breeze::fastlua
