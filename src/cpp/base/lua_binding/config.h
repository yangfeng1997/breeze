#pragma once

// breeze::lua_binding：Lua 5.5 的 C++ 绑定库。
// 这个头只做两件事：把 Lua 的 C 头按 C 链接引入，集中标准库依赖，并在编译期卡版本。
// 任何 public 头都必须先包含它，而不是各自 include <lua.h>。

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#if !defined(LUA_VERSION_NUM) || LUA_VERSION_NUM < 505
#error "breeze::lua_binding requires Lua 5.5 or newer"
#endif

// Lua 按 C 编译时 lua_error 是 longjmp，跨过带析构的 C++ 栈帧会跳过析构。
// 因此 breeze::lua_binding 的核心不抛异常：错误走 Result / ErrorSlot，lua_error 只在
// 两段式 trampoline 的“只有平凡局部变量”的那一层调用（见 trampoline.h）。
// 如果 Lua 按 C++ 编译（vcpkg lua 的 cpp feature），lua_error 会变成异常，
// 这一层假设就不成立，所以这里直接拒绝那种构建。
#if defined(__cplusplus) && defined(LUA_USE_CXX_EXCEPTIONS)
#error "breeze::lua_binding requires Lua compiled as C (do not enable the vcpkg lua cpp feature)"
#endif

namespace breeze::lua_binding {

inline constexpr bool kDebugBuild =
#if defined(NDEBUG)
    false;
#else
    true;
#endif

}  // namespace breeze::lua_binding
