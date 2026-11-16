#pragma once

#include "base/lua_binding/config.h"

namespace breeze::lua_binding {

void OpenTableExt(lua_State* L);
void OpenStringExt(lua_State* L);
void OpenOsExt(lua_State* L);

}  // namespace breeze::lua_binding
