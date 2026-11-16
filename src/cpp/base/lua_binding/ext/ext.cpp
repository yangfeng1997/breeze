#include "base/lua_binding/ext/ext.h"

#include "base/lua_binding/ext/ext_impl.h"

namespace breeze::lua_binding {

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

}  // namespace breeze::lua_binding
