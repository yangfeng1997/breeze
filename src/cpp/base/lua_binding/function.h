#pragma once

// LuaFunction：registry 函数引用；LuaFunctionView：栈上的函数视图。

#include <utility>

#include "base/lua_binding/call.h"
#include "base/lua_binding/check.h"
#include "base/lua_binding/config.h"
#include "base/lua_binding/error.h"
#include "base/lua_binding/reference.h"

namespace breeze::lua_binding {

class LuaFunction : public Reference {
public:
    LuaFunction() noexcept = default;

    // 独立引用：与基类 Clone 一样新建 registry 槽，只在 owner 线程调用。
    LuaFunction Clone() const { return this->template CloneAs<LuaFunction>(); }

    static LuaFunction FromStack(lua_State* L, int index) {
        BREEZE_LUA_CHECK(lua_isfunction(L, index), "breeze::lua_binding: LuaFunction::FromStack expects a function");
        LuaFunction out;
        static_cast<Reference&>(out) = Reference::FromStack(L, index);
        return out;
    }

    template <class R, class... Args>
    Result<R> Call(Args&&... args) const {
        lua_State* L = State();
        if (L == nullptr) {
            return Error{ErrorCode::kRuntime, "breeze::lua_binding: LuaFunction used after its LuaState was closed"};
        }
        return CallRef<R>(L, ref_, std::forward<Args>(args)...);
    }

    // 热路径：不挂 traceback。错误仍然由 pcall 捕获并返回 Result。
    template <class R, class... Args>
    Result<R> CallNoTraceback(Args&&... args) const {
        lua_State* L = State();
        if (L == nullptr) {
            return Error{ErrorCode::kRuntime, "breeze::lua_binding: LuaFunction used after its LuaState was closed"};
        }
        return CallRefNoTraceback<R>(L, ref_, std::forward<Args>(args)...);
    }

    // 逃生门：lua_call，不捕获错误。只有确认 Lua 函数不会报错时才用；
    // 报错会 longjmp，跳过当前 C++ 栈上的析构。返回值类型不匹配直接 Fatal。
    template <class R, class... Args>
    R CallUnprotected(Args&&... args) const {
        lua_State* L = State();
        BREEZE_LUA_CHECK(L != nullptr, "breeze::lua_binding: LuaFunction used after its LuaState was closed");
        return CallRefUnprotected<R>(L, ref_, std::forward<Args>(args)...);
    }
};


struct LuaFunctionView {
    lua_State* L = nullptr;
    int index = 0;

    template <class R, class... Args>
    Result<R> Call(Args&&... args) const {
        const int abs = lua_absindex(L, index);
        lua_pushvalue(L, abs);
        return CallPushed<R>(L, lua_gettop(L), std::forward<Args>(args)...);
    }
};

}  // namespace breeze::lua_binding
