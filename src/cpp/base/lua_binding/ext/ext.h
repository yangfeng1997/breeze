#pragma once

// 可选扩展：table / string / os 的常用工具。单独链接 breeze_lua_binding_ext，
// 不用的人不会为这些函数付体积。实现重写自 luakit 的扩展，修掉了硬编码栈索引、
// 循环引用、number key 上 lua_tolstring 等 bug。

#include "base/lua_binding/lua_state.h"

namespace breeze::lua_binding {

enum class Ext : unsigned {
    kTable = 1U << 0,
    kString = 1U << 1,
    kOs = 1U << 2,
    kAll = kTable | kString | kOs,
};

void OpenExt(LuaStateView lua, Ext flags);

}  // namespace breeze::lua_binding
