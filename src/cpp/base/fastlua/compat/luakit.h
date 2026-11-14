#pragma once

// luakit 风格的兼容入口：只做名字转发，不复制实现。
// 迁移 quanta 脚本/注册代码时把 include 换成这个头即可，核心 API 仍是 PascalCase。
//
// 故意不提供 luakit 的 lua_codec / lua_buff / lua_slice（encode / decode / serialize）：
// 那是 RPC 编解码与缓冲区，属于独立包，不属于 Lua 绑定库。
//
//   lua_compat::new_class<Player>(lua, "Player", "hp", &Player::Hp, "name", &Player::name);
//   lua_compat::new_enum(lua, "Color", "Red", 1, "Green", 2);

#include <string_view>
#include <utility>

#include "base/fastlua/class.h"
#include "base/fastlua/lua_state.h"
#include "base/fastlua/ref.h"

namespace breeze::fastlua::compat {

template <class Builder>
void AddMembers(Builder&) {}

template <class Builder, class M, class... Rest>
void AddMembers(Builder& builder, std::string_view name, M member, Rest&&... rest) {
    builder.Add(name, member);
    AddMembers(builder, std::forward<Rest>(rest)...);
}

template <class T, class... Args>
void new_class(LuaStateView lua, std::string_view name, Args&&... args) {
    static_assert(sizeof...(Args) % 2 == 0, "fastlua compat: new_class expects name/member pairs");
    auto builder = lua.Class<T>(name);
    AddMembers(builder, std::forward<Args>(args)...);
}

inline void AddEnumValues(lua_State* /*L*/) {}

template <class V, class... Rest>
void AddEnumValues(lua_State* L, std::string_view name, V value, Rest&&... rest) {
    lua_pushlstring(L, name.data(), name.size());
    Stack<V>::Push(L, value);
    lua_settable(L, -3);
    AddEnumValues(L, std::forward<Rest>(rest)...);
}

template <class... Args>
void new_enum(LuaStateView lua, std::string_view name, Args&&... args) {
    static_assert(sizeof...(Args) % 2 == 0, "fastlua compat: new_enum expects name/value pairs");
    lua_State* L = lua.Raw();
    lua_newtable(L);
    AddEnumValues(L, std::forward<Args>(args)...);
    const std::string key(name);
    lua_setglobal(L, key.c_str());
}

template <class R, class... Args>
Result<R> call(LuaStateView lua, std::string_view name, Args&&... args) {
    return lua.Call<R>(name, std::forward<Args>(args)...);
}

template <class R, class... Args>
Result<R> table_call(LuaStateView lua, std::string_view table, std::string_view function, Args&&... args) {
    Result<Table> target = lua.GetTable(table);
    if (!target.Ok()) {
        return target.GetError();
    }
    return target.Value().Call<R>(function, std::forward<Args>(args)...);
}

inline Table new_table(LuaStateView lua, std::string_view name) {
    lua_State* L = lua.Raw();
    lua_newtable(L);
    const int index = lua_gettop(L);
    lua_pushvalue(L, index);
    const std::string key(name);
    lua_setglobal(L, key.c_str());
    Table table = Table::FromStack(L, index);
    lua_pop(L, 1);
    return table;
}

template <class F>
void set_function(LuaStateView lua, std::string_view name, F&& function) {
    lua.SetFunction(name, std::forward<F>(function));
}

template <class T>
void set(LuaStateView lua, std::string_view name, T&& value) {
    lua.Set(name, std::forward<T>(value));
}

template <class T>
Result<T> get(LuaStateView lua, std::string_view name) {
    return lua.Get<T>(name);
}

}  // namespace breeze::fastlua::compat
