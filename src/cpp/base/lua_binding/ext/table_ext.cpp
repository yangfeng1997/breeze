#include "base/lua_binding/ext/ext_impl.h"

#include <cstring>

namespace breeze::lua_binding {
namespace {

int TableSize(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushinteger(L, static_cast<lua_Integer>(lua_rawlen(L, 1)));
    return 1;
}

int TableCopy(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_newtable(L);
    const int dst = lua_gettop(L);
    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
        lua_pushvalue(L, -2);
        lua_pushvalue(L, -2);
        lua_settable(L, dst);
        lua_pop(L, 1);
    }
    return 1;
}

constexpr int kMaxCopyDepth = 64;

void DeepCopyValue(lua_State* L, int index, int depth, int visited) {
    index = lua_absindex(L, index);
    if (lua_type(L, index) != LUA_TTABLE) {
        lua_pushvalue(L, index);
        return;
    }
    if (depth > kMaxCopyDepth) {
        luaL_error(L, "breeze::lua_binding: table.deepcopy exceeded depth %d", kMaxCopyDepth);
    }
    lua_pushvalue(L, index);
    lua_rawget(L, visited);
    if (!lua_isnil(L, -1)) {
        return;
    }
    lua_pop(L, 1);

    lua_newtable(L);
    const int dst = lua_gettop(L);
    lua_pushvalue(L, index);
    lua_pushvalue(L, dst);
    lua_rawset(L, visited);

    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        lua_pushvalue(L, -2);  // key
        lua_pushvalue(L, -2);  // value
        DeepCopyValue(L, -1, depth + 1, visited);  // pushes copy
        lua_remove(L, -2);     // remove original value
        lua_rawset(L, dst);
        lua_pop(L, 1);         // pop value, keep key
    }
}

int TableDeepCopy(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_newtable(L);
    const int visited = lua_gettop(L);
    DeepCopyValue(L, 1, 0, visited);
    lua_remove(L, visited);
    return 1;
}

int TableKeys(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_newtable(L);
    const int dst = lua_gettop(L);
    lua_Integer i = 0;
    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
        lua_pushvalue(L, -2);
        lua_rawseti(L, dst, ++i);
        lua_pop(L, 1);
    }
    return 1;
}

int TableVals(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_newtable(L);
    const int dst = lua_gettop(L);
    lua_Integer i = 0;
    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
        lua_pushvalue(L, -1);
        lua_rawseti(L, dst, ++i);
        lua_pop(L, 1);
    }
    return 1;
}

int TableMerge(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_pushnil(L);
    while (lua_next(L, 2) != 0) {
        lua_pushvalue(L, -2);
        lua_pushvalue(L, -2);
        lua_settable(L, 1);
        lua_pop(L, 1);
    }
    lua_pushvalue(L, 1);
    return 1;
}

}  // namespace

void OpenTableExt(lua_State* L) {
    lua_getglobal(L, "table");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    const struct {
        const char* name;
        lua_CFunction fn;
    } functions[] = {
        {"size", TableSize},
        {"copy", TableCopy},
        {"deepcopy", TableDeepCopy},
        {"keys", TableKeys},
        {"vals", TableVals},
        {"merge", TableMerge},
    };
    for (const auto& entry : functions) {
        lua_pushcfunction(L, entry.fn);
        lua_setfield(L, -2, entry.name);
    }
    lua_pop(L, 1);
}

}  // namespace breeze::lua_binding
