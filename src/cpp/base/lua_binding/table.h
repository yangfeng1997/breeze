#pragma once

// LuaTable：registry 表引用；LuaTableView：栈上的表视图（只在当前 C 函数内有效）。

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "base/lua_binding/call.h"
#include "base/lua_binding/check.h"
#include "base/lua_binding/config.h"
#include "base/lua_binding/error.h"
#include "base/lua_binding/reference.h"
#include "base/lua_binding/stack.h"

namespace breeze::lua_binding {

class LuaTable : public Reference {
public:
    LuaTable() noexcept = default;

    // 独立引用：与基类 Clone 一样新建 registry 槽，只在 owner 线程调用。
    LuaTable Clone() const { return this->template CloneAs<LuaTable>(); }

    static LuaTable FromStack(lua_State* L, int index) {
        BREEZE_LUA_CHECK(lua_istable(L, index), "breeze::lua_binding: LuaTable::FromStack expects a table");
        LuaTable out;
        static_cast<Reference&>(out) = Reference::FromStack(L, index);
        return out;
    }

    template <class T>
    std::expected<T, Error> Get(std::string_view key) const {
        lua_State* L = State();
        if (L == nullptr) {
            return Fail(ErrorCode::kInvalidState, "breeze::lua_binding: LuaTable used after its LuaState was closed");
        }
        const int top = lua_gettop(L);
        Push();
        const int table_index = top + 1;
        lua_pushlstring(L, key.data(), key.size());
        lua_gettable(L, table_index);

        if (lua_isnil(L, -1)) {
            if (Stack<T>::Check(L, -1)) {
                T value = Stack<T>::Get(L, -1);
                lua_settop(L, top);
                return value;
            }
            lua_settop(L, top);
            return Fail(ErrorCode::kNotFound, "breeze::lua_binding: key '" + std::string(key) + "' not found");
        }
        if (!Stack<T>::Check(L, -1)) {
            const std::string actual = luaL_typename(L, -1);
            lua_settop(L, top);
            return Fail(ErrorCode::kType, "breeze::lua_binding: key '" + std::string(key) + "' expects " + Stack<T>::kName +
                                               ", got " + actual);
        }
        T value = Stack<T>::Get(L, -1);
        lua_settop(L, top);
        return value;
    }

    template <class T>
    void Set(std::string_view key, T&& value) const {
        lua_State* L = State();
        BREEZE_LUA_CHECK(L != nullptr, "breeze::lua_binding: LuaTable used after its LuaState was closed");
        const int top = lua_gettop(L);
        Push();
        lua_pushlstring(L, key.data(), key.size());
        Stack<std::decay_t<T>>::Push(L, std::forward<T>(value));
        lua_settable(L, top + 1);
        lua_settop(L, top);
    }

    template <class R, class... Args>
    std::expected<R, Error> Call(std::string_view function, Args&&... args) const {
        lua_State* L = State();
        if (L == nullptr) {
            return Fail(ErrorCode::kInvalidState, "breeze::lua_binding: LuaTable used after its LuaState was closed");
        }
        const int top = lua_gettop(L);
        Push();
        lua_getfield(L, top + 1, std::string(function).c_str());
        if (!lua_isfunction(L, -1)) {
            lua_settop(L, top);
            return Fail(ErrorCode::kNotFound, "breeze::lua_binding: table function '" + std::string(function) + "' not found");
        }
        std::expected<R, Error> result = CallPushed<R>(L, lua_gettop(L), std::forward<Args>(args)...);
        lua_settop(L, top);
        return result;
    }
};


// 栈视图：只在当前 C 函数内有效。
struct LuaTableView {
    lua_State* L = nullptr;
    int index = 0;

    // 热路径：key 是字面量时用 lua_getfield，避免每次 push 一个 Lua 字符串；
    // 返回 bool 而不是 std::expected，不构造错误消息。
    template <class T>
    bool TryGetField(const char* key, T& value) const {
        const int abs = lua_absindex(L, index);
        const int top = lua_gettop(L);
        lua_getfield(L, abs, key);
        if (!Stack<T>::Check(L, -1)) {
            lua_settop(L, top);
            return false;
        }
        value = Stack<T>::Get(L, -1);
        lua_settop(L, top);
        return true;
    }

    template <class T>
    std::expected<T, Error> Get(std::string_view key) const {
        const int abs = lua_absindex(L, index);
        const int top = lua_gettop(L);
        lua_pushlstring(L, key.data(), key.size());
        lua_gettable(L, abs);
        if (lua_isnil(L, -1) && !Stack<T>::Check(L, -1)) {
            lua_settop(L, top);
            return Fail(ErrorCode::kNotFound, "breeze::lua_binding: key '" + std::string(key) + "' not found");
        }
        if (!Stack<T>::Check(L, -1)) {
            const std::string actual = luaL_typename(L, -1);
            lua_settop(L, top);
            return Fail(ErrorCode::kType, "breeze::lua_binding: key '" + std::string(key) + "' expects " + Stack<T>::kName +
                                               ", got " + actual);
        }
        T value = Stack<T>::Get(L, -1);
        lua_settop(L, top);
        return value;
    }

    template <class T>
    void Set(std::string_view key, T&& value) const {
        const int abs = lua_absindex(L, index);
        const int top = lua_gettop(L);
        lua_pushlstring(L, key.data(), key.size());
        Stack<std::decay_t<T>>::Push(L, std::forward<T>(value));
        lua_settable(L, abs);
        lua_settop(L, top);
    }
};

// 栈视图作为参数：不占 registry，只在当前 C 函数内有效。
template <>
struct Stack<LuaTableView, void> {
    static constexpr const char* kName = "table";

    static bool Check(lua_State* L, int index) { return lua_istable(L, index); }
    static LuaTableView Get(lua_State* L, int index) { return LuaTableView{L, lua_absindex(L, index)}; }
};

}  // namespace breeze::lua_binding
