#pragma once

// 沙箱与限额：
//   - 库白名单：构造 LuaState 时用 luaL_openselectedlibs 只开需要的库。
//   - 内存上限：自定义 lua_Alloc，同时统计分配次数（“热路径零分配”测试也用它）。
//   - 指令/时间预算：lua_sethook + LUA_MASKCOUNT；超预算后把计数设为 1，防止
//     `while true do pcall(f) end` 吞掉一次错误后继续跑。

#include <cstddef>
#include <cstdint>

#include "base/fastlua/lua_state.h"

namespace breeze::fastlua {

struct AllocStats {
    std::size_t used = 0;
    std::size_t limit = 0;
    std::size_t allocations = 0;
    std::size_t failures = 0;
};

extern "C" void* SandboxAlloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize);

inline constexpr unsigned kSandboxSafeLibs =
    LUA_GLIBK | LUA_COLIBK | LUA_MATHLIBK | LUA_STRLIBK | LUA_TABLIBK | LUA_UTF8LIBK;

struct SandboxOptions {
    unsigned libs = kSandboxSafeLibs;
    std::size_t memory_limit = 0;       // 0 表示不限
    std::uint64_t instruction_budget = 0;  // 0 表示不限
    std::uint64_t time_budget_ms = 0;      // 0 表示不限
};

LuaStateOptions MakeSandboxOptions(const SandboxOptions& options, AllocStats& stats);
void ApplySandbox(LuaState& lua, const SandboxOptions& options);

}  // namespace breeze::fastlua
