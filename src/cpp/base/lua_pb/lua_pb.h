// lua_pb 的 Lua 打包层：把 Codec 包成 require("pb") 的模块。设计见 docs/Lua_pb库设计.md §0（结论 7）。
//
//   local pb = require("pb")
//   local bytes = pb.encode("pkg.Msg", { ... })   -- 失败抛 Lua 错误
//   local tbl   = pb.decode("pkg.Msg", bytes)
//
// 规矩：
//   - 这一层只给脚本 / worker / 测试用；**生产路径（RpcModule 的 Lua 适配器）直接用 C++ API**，
//     拿 std::expected，不产生 Lua 错误、不需要 pcall；
//   - 失败一律 luaL_error（与 starwing / quanta 一致），坏字节在脚本里必须 pcall；
//   - Codec 放在 userdata 里，靠 __gc 析构——这样"Codec 必须先于 lua_State 关闭而析构"这条规则
//     由 GC 顺序保证，宿主不用自己管。

#pragma once

#include <memory>
#include <utility>

#include "base/lua_binding/lua_state.h" // LuaStateView / LuaState（只用到 Raw()）
#include "base/lua_pb/codec.h"
#include "base/lua_pb/error.h"
#include "base/lua_pb/schema.h"
#include "base/lua_pb/wire.h"

namespace breeze::lua_pb
{

// 在当前 lua_State 里注册模块 "pb"（写进 package.loaded），schema 由模块持有。
// 同一个 state 重复调用会替换掉旧模块（旧的 Codec 由 GC 回收）。
void OpenPb(lua_State* L, std::shared_ptr<const Schema> schema);

inline void OpenPb(lua_binding::LuaStateView L, std::shared_ptr<const Schema> schema)
{
    OpenPb(L.Raw(), std::move(schema));
}

} // namespace breeze::lua_pb
