#include "base/fastlua/ext/ext.h"

#include "base/fastlua/ext/ext_impl.h"

namespace breeze::fastlua {

void OpenExt(LuaStateView lua, Ext flags) {
    lua_State* L = lua.Raw();
    const unsigned value = static_cast<unsigned>(flags);
    if ((value & static_cast<unsigned>(Ext::kTable)) != 0) {
        OpenTableExt(L);
    }
    if ((value & static_cast<unsigned>(Ext::kString)) != 0) {
        OpenStringExt(L);
    }
    if ((value & static_cast<unsigned>(Ext::kOs)) != 0) {
        OpenOsExt(L);
    }
}

}  // namespace breeze::fastlua
