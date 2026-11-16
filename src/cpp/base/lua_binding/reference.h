#pragma once

// registry 引用基类：Reference 的生命周期与线程规则。
//
// 线程规则（必须遵守）：Reference / LuaTable / LuaFunction 必须在创建它的线程（owner）上析构。
// 异步 worker / IO / DB 线程只传 ID 或令牌，不传 Reference；任意线程可析构的 C++ 句柄
// （如 ReplyHandle）不得直接持有 Reference。违反是用法错误：这里不碰 VM，只记录一次并
// 泄漏该 registry 槽。库刻意不实现跨线程释放队列——sol2、LuaBridge3、luakit 同样不做。
//
// Control / StateData 在 state_data.h；LuaTable / LuaFunction 分别在 table.h / function.h。

#include <thread>

#include "base/lua_binding/check.h"
#include "base/lua_binding/config.h"
#include "base/lua_binding/state_data.h"

namespace breeze::lua_binding {

class Reference {
public:
    Reference() noexcept = default;
    Reference(const Reference&) = delete;
    Reference& operator=(const Reference&) = delete;
    Reference(Reference&& other) noexcept : control_(other.control_), ref_(other.ref_) {
        other.control_ = nullptr;
        other.ref_ = LUA_NOREF;
    }
    Reference& operator=(Reference&& other) noexcept {
        if (this != &other) {
            Reset();
            control_ = other.control_;
            ref_ = other.ref_;
            other.control_ = nullptr;
            other.ref_ = LUA_NOREF;
        }
        return *this;
    }
    ~Reference() { Reset(); }

    bool Valid() const noexcept { return control_ != nullptr && State() != nullptr; }

    // 显式复制：为同一个 Lua 值新建一个 registry 槽，两个 Reference 的生命周期互相独立。
    // 必须在 owner 线程调用（和 Push 一样要访问 VM）；空引用返回空，state 已关闭是用法错误。
    // 不要用 Clone 做热路径缓存：只读场景用 const Reference& / LuaTableView / LuaFunctionView。
    Reference Clone() const { return CloneAs<Reference>(); }

    int Push() const {
        lua_State* L = State();
        BREEZE_LUA_CHECK(L != nullptr, "breeze::lua_binding: Reference used after its LuaState was closed");
        PushTo(L);
        return lua_gettop(L);
    }

    static Reference FromStack(lua_State* L, int index) {
        index = lua_absindex(L, index);
        lua_pushvalue(L, index);
        StateData* data = GetOrCreateStateData(L);
        data->control->AddRef();
        Reference out;
        out.control_ = data->control;
        out.ref_ = luaL_ref(L, LUA_REGISTRYINDEX);
        return out;
    }

protected:
    lua_State* State() const noexcept {
        return control_ != nullptr ? control_->state.load(std::memory_order_acquire) : nullptr;
    }

    // 调用方已经拿到 L 时用它，避免重复的原子 load 与 CHECK。
    void PushTo(lua_State* L) const noexcept { lua_rawgeti(L, LUA_REGISTRYINDEX, ref_); }

    template <class Self>
    Self CloneAs() const {
        if (control_ == nullptr) {
            return Self{};
        }
        lua_State* L = State();
        BREEZE_LUA_CHECK(L != nullptr, "breeze::lua_binding: Reference::Clone after its LuaState was closed");
        if (ref_ == LUA_NOREF || ref_ == LUA_REFNIL) {
            return Self{};
        }
        Push();
        Self out = Self::FromStack(L, -1);
        lua_pop(L, 1);
        return out;
    }

    void Reset() noexcept {
        if (control_ != nullptr) {
            lua_State* L = control_->state.load(std::memory_order_acquire);
            if (L != nullptr && ref_ != LUA_NOREF && ref_ != LUA_REFNIL) {
                if (std::this_thread::get_id() == control_->owner) {
                    luaL_unref(L, LUA_REGISTRYINDEX, ref_);
                } else {
                    // 用法错误：Reference 必须在 owner 线程析构。不碰 VM，只记录一次并泄漏槽。
                    BREEZE_LUA_ENSURE(false,
                                   "breeze::lua_binding: Reference destroyed on a non-owner thread; leaking registry slot "
                                   "(pass IDs across threads, not Reference)");
                }
            }
            control_->Release();
            control_ = nullptr;
            ref_ = LUA_NOREF;
        }
    }

    Control* control_ = nullptr;
    int ref_ = LUA_NOREF;
};

}  // namespace breeze::lua_binding
