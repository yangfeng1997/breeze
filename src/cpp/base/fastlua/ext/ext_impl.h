#pragma once

#include "base/fastlua/config.h"

namespace breeze::fastlua {

void OpenTableExt(lua_State* L);
void OpenStringExt(lua_State* L);
void OpenOsExt(lua_State* L);

}  // namespace breeze::fastlua
