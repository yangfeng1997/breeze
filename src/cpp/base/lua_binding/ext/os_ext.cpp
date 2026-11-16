#include "base/lua_binding/ext/ext_impl.h"

#include <cstdlib>

namespace breeze::lua_binding {
namespace {

int OsSetEnv(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const char* value = luaL_checkstring(L, 2);
#ifdef _WIN32
    const int result = _putenv_s(name, value);
#else
    const int result = ::setenv(name, value, 1);
#endif
    lua_pushboolean(L, result == 0);
    return 1;
}

int OsGetEnv(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const char* value = std::getenv(name);
    if (value != nullptr) {
        lua_pushstring(L, value);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

}  // namespace

void OpenOsExt(lua_State* L) {
    lua_getglobal(L, "os");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_pushcfunction(L, OsSetEnv);
    lua_setfield(L, -2, "setenv");
    lua_pushcfunction(L, OsGetEnv);
    lua_setfield(L, -2, "getenv");
    lua_pop(L, 1);
}

}  // namespace breeze::lua_binding
