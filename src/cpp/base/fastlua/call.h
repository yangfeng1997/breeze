#pragma once

// C++ 调 Lua。
// 每次调用：push 一个 light C 错误处理函数 → push 函数 → push 参数 → lua_pcall → settop。
// 错误处理函数没有 upvalue，lua_pushcfunction 不分配；它只在出错时生成 traceback。

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

// 函数已经在栈上（func_index）。压 msgh、把 msgh 移到函数下面，再调用。
// 与 CallGlobal 的区别只是“函数从哪来”。
template <class R, class... Args>
Result<R> CallPushed(lua_State* L, int func_index, Args&&... args) {
    const int base = lua_absindex(L, func_index);
    lua_pushcfunction(L, &MessageHandler);
    lua_insert(L, base);  // [.., msgh, function]

    if (!lua_checkstack(L, static_cast<int>(sizeof...(Args)) + 4)) {
        lua_settop(L, base - 1);
        return Error{ErrorCode::kStack, "fastlua: Lua stack overflow while pushing arguments"};
    }
    PushArgs(L, std::forward<Args>(args)...);

    const int status = lua_pcall(L, static_cast<int>(sizeof...(Args)), ReturnCount<R>(), base);
    if (status != LUA_OK) {
        std::string message = TakeError(L, base - 1);
        return Error{status == LUA_ERRMEM ? ErrorCode::kMemory : ErrorCode::kRuntime, std::move(message)};
    }

    using D = std::remove_cvref_t<R>;
    if constexpr (std::is_void_v<D>) {
        lua_settop(L, base - 1);
        return Ok();
    } else if constexpr (is_tuple_like_v<D>) {
        const int first = lua_gettop(L) - ReturnCount<R>() + 1;
        D value = ReadTuple<D>(L, first, std::make_index_sequence<std::tuple_size_v<D>>{});
        lua_settop(L, base - 1);
        return value;
    } else {
        const int first = lua_gettop(L);
        D value = Stack<D>::Get(L, first);
        lua_settop(L, base - 1);
        return value;
    }
}

// 快路径：仍然 pcall（不会把错误变成 panic），但不挂 traceback、不 lua_insert。
// 适合“错误由上层处理、热路径不关心 traceback”的调用。
template <class R, class... Args>
Result<R> CallPushedFast(lua_State* L, int func_index, Args&&... args) {
    const int base = lua_absindex(L, func_index);
    PushArgs(L, std::forward<Args>(args)...);
    const int status = lua_pcall(L, static_cast<int>(sizeof...(Args)), ReturnCount<R>(), 0);
    if (status != LUA_OK) {
        std::string message = TakeError(L, base - 1);
        return Error{status == LUA_ERRMEM ? ErrorCode::kMemory : ErrorCode::kRuntime, std::move(message)};
    }

    using D = std::remove_cvref_t<R>;
    if constexpr (std::is_void_v<D>) {
        lua_settop(L, base - 1);
        return Ok();
    } else if constexpr (is_tuple_like_v<D>) {
        const int first = lua_gettop(L) - ReturnCount<R>() + 1;
        D value = ReadTuple<D>(L, first, std::make_index_sequence<std::tuple_size_v<D>>{});
        lua_settop(L, base - 1);
        return value;
    } else {
        const int first = lua_gettop(L);
        D value = Stack<D>::Get(L, first);
        lua_settop(L, base - 1);
        return value;
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
Result<R> CallGlobal(lua_State* L, std::string_view name, Args&&... args) {
    const int base = lua_gettop(L);
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

    if (!lua_checkstack(L, static_cast<int>(sizeof...(Args)) + 4)) {
        lua_settop(L, base);
        return Error{ErrorCode::kStack, "fastlua: Lua stack overflow while pushing arguments"};
    }
    PushArgs(L, std::forward<Args>(args)...);

    const int status = lua_pcall(L, static_cast<int>(sizeof...(Args)), ReturnCount<R>(), msgh);
    if (status != LUA_OK) {
        std::string message = TakeError(L, base);
        return Error{status == LUA_ERRMEM ? ErrorCode::kMemory : ErrorCode::kRuntime, std::move(message)};
    }

    using D = std::remove_cvref_t<R>;
    if constexpr (std::is_void_v<D>) {
        lua_settop(L, base);
        return Ok();
    } else if constexpr (is_tuple_like_v<D>) {
        const int first = lua_gettop(L) - ReturnCount<R>() + 1;
        D value = ReadTuple<D>(L, first, std::make_index_sequence<std::tuple_size_v<D>>{});
        lua_settop(L, base);
        return value;
    } else {
        const int first = lua_gettop(L) - 1 + 1;
        D value = Stack<D>::Get(L, first);
        lua_settop(L, base);
        return value;
    }
}

}  // namespace breeze::fastlua
