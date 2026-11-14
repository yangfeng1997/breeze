#include "base/fastlua/class.h"

namespace breeze::fastlua {

int ClassGc(lua_State* L) {
    auto* header = static_cast<ObjHeader*>(lua_touserdata(L, 1));
    if (header != nullptr && header->cookie == kObjCookie) {
        if (header->destroy_holder != nullptr) {
            // shared_ptr 持有块：只释放引用，不 delete obj。
            header->destroy_holder(header->holder);
            header->holder = nullptr;
            header->destroy_holder = nullptr;
        } else if ((header->flags & kObjOwned) != 0 && header->cls != nullptr && header->cls->destroy != nullptr) {
            header->cls->destroy(header);
        }
        header->cookie = 0;
        header->obj = nullptr;
    }
    return 0;
}

int ClassIndex(lua_State* L) {
    auto* cls = static_cast<ClassInfo*>(lua_touserdata(L, lua_upvalueindex(3)));

    // 方法：先从 methods 表取，命中就是零额外分配（除非开了 dot-call 兼容模式）。
    lua_pushvalue(L, 2);
    lua_rawget(L, lua_upvalueindex(1));
    if (lua_isfunction(L, -1)) {
        if (cls != nullptr && cls->dot_call) {
            lua_pushvalue(L, 1);  // self
            lua_pushcclosure(L, &DotThunk, 2);
        }
        return 1;
    }
    lua_pop(L, 1);

    // 属性：getters 表里存 lightuserdata PropDesc*。
    lua_pushvalue(L, 2);
    lua_rawget(L, lua_upvalueindex(2));
    if (!lua_isnil(L, -1)) {
        auto* desc = static_cast<PropDesc*>(lua_touserdata(L, -1));
        lua_pop(L, 1);
        ObjHeader* header = GetHeader(L, 1);
        if (header == nullptr || desc == nullptr) {
            luaL_error(L, "fastlua: invalid property access");
        }
        void* obj = desc->cast != nullptr ? desc->cast(header->obj) : header->obj;
        desc->getter(L, obj);
        return 1;
    }
    lua_pop(L, 1);

    // Lua 侧动态字段（AllowFields）。
    if (cls != nullptr && cls->allow_fields) {
        lua_getiuservalue(L, 1, 1);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_pushnil(L);
            return 1;
        }
        lua_pushvalue(L, 2);
        lua_rawget(L, -2);
        lua_remove(L, -2);
        return 1;
    }

    lua_pushnil(L);
    return 1;
}

int ClassNewIndex(lua_State* L) {
    auto* cls = static_cast<ClassInfo*>(lua_touserdata(L, lua_upvalueindex(2)));

    lua_pushvalue(L, 2);
    lua_rawget(L, lua_upvalueindex(1));
    if (!lua_isnil(L, -1)) {
        auto* desc = static_cast<PropDesc*>(lua_touserdata(L, -1));
        lua_pop(L, 1);
        ObjHeader* header = GetHeader(L, 1);
        if (header == nullptr || desc == nullptr) {
            luaL_error(L, "fastlua: invalid property assignment");
        }
        void* obj = desc->cast != nullptr ? desc->cast(header->obj) : header->obj;
        desc->setter(L, obj);
        return 0;
    }
    lua_pop(L, 1);

    if (cls != nullptr && cls->allow_fields) {
        lua_getiuservalue(L, 1, 1);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushvalue(L, -1);
            lua_setiuservalue(L, 1, 1);
        }
        lua_pushvalue(L, 2);
        lua_pushvalue(L, 3);
        lua_rawset(L, -3);
        lua_pop(L, 1);
        return 0;
    }

    const char* key = lua_tostring(L, 2);
    luaL_error(L, "fastlua: attempt to set unknown field '%s' on %s", key != nullptr ? key : "?",
               cls != nullptr ? cls->name.c_str() : "object");
    return 0;
}

int DotThunk(lua_State* L) {
    const int nargs = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));  // method
    lua_pushvalue(L, lua_upvalueindex(2));  // self
    for (int i = 1; i <= nargs; ++i) {
        lua_pushvalue(L, i);
    }
    lua_call(L, nargs + 1, LUA_MULTRET);
    return lua_gettop(L) - nargs;
}

void CopyMissing(lua_State* L, int from_ref, int to_ref) {
    const int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, from_ref);
    const int from = top + 1;
    lua_rawgeti(L, LUA_REGISTRYINDEX, to_ref);
    const int to = top + 2;

    lua_pushnil(L);
    while (lua_next(L, from) != 0) {
        // [key][value]
        lua_pushvalue(L, -2);
        lua_rawget(L, to);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_pushvalue(L, -2);  // key
            lua_pushvalue(L, -2);  // value
            lua_rawset(L, to);
        } else {
            lua_pop(L, 1);
        }
        lua_pop(L, 1);  // pop value, keep key
    }
    lua_settop(L, top);
}

void CopyMetamethods(lua_State* L, const ClassInfo* base, const ClassInfo* derived) {
    static constexpr const char* kNames[] = {"__tostring", "__eq", "__lt", "__close"};
    const int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, base->mt_ref);
    const int from = top + 1;
    lua_rawgeti(L, LUA_REGISTRYINDEX, derived->mt_ref);
    const int to = top + 2;

    for (const char* name : kNames) {
        lua_getfield(L, from, name);
        if (!lua_isnil(L, -1)) {
            lua_getfield(L, to, name);
            if (lua_isnil(L, -1)) {
                lua_pop(L, 1);
                lua_pushvalue(L, -1);
                lua_setfield(L, to, name);
            } else {
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }
    lua_settop(L, top);
}

void CopyProps(lua_State* L, const ClassInfo* base, ClassInfo* derived, void* (*cast)(void*)) {
    const int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, base->getters_ref);
    const int from = top + 1;

    lua_pushnil(L);
    while (lua_next(L, from) != 0) {
        // [key][value(desc)]
        lua_rawgeti(L, LUA_REGISTRYINDEX, derived->getters_ref);
        const int to_getters = lua_gettop(L);
        lua_pushvalue(L, -3);  // key
        lua_rawget(L, to_getters);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            auto* source = static_cast<PropDesc*>(lua_touserdata(L, -2));
            if (source != nullptr) {
                auto* copy = new PropDesc;
                copy->cast = cast;
                copy->getter = source->getter;
                copy->setter = source->setter;
                derived->props.push_back(copy);

                lua_pushvalue(L, -3);  // key
                lua_pushlightuserdata(L, copy);
                lua_rawset(L, to_getters);

                lua_rawgeti(L, LUA_REGISTRYINDEX, derived->setters_ref);
                const int to_setters = lua_gettop(L);
                lua_pushvalue(L, -4);  // key
                lua_pushlightuserdata(L, copy);
                lua_rawset(L, to_setters);
                lua_pop(L, 1);  // to_setters
            }
        } else {
            lua_pop(L, 1);
        }
        lua_pop(L, 1);  // to_getters
        lua_pop(L, 1);  // value, keep key
    }
    lua_settop(L, top);
}

}  // namespace breeze::fastlua
