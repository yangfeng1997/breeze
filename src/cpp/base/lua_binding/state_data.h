#pragma once

// lua_State 的内部共享数据：Control 引用计数块 + StateData（类表、state 指针）。
// 只供 reference.h / object.h / class.h 内部使用，不属于业务 API。

#include <atomic>
#include <new>
#include <thread>
#include <unordered_map>

#include "base/lua_binding/check.h"
#include "base/lua_binding/class_info.h"
#include "base/lua_binding/config.h"

namespace breeze::lua_binding {

class Control {
public:
    std::atomic<int> refs{1};
    std::atomic<lua_State*> state{nullptr};
    std::thread::id owner;

    void AddRef() noexcept { refs.fetch_add(1, std::memory_order_relaxed); }
    void Release() noexcept {
        if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }
};

inline lua_State* MainState(lua_State* L) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
    lua_State* main = lua_tothread(L, -1);
    lua_pop(L, 1);
    return main != nullptr ? main : L;
}

struct StateData {
    Control* control = nullptr;
    std::unordered_map<const void*, ClassInfo*> classes;

    static int Gc(lua_State* L) {
        auto* data = static_cast<StateData*>(lua_touserdata(L, 1));
        if (data != nullptr) {
            for (auto& [tag, cls] : data->classes) {
                (void)tag;
                for (PropertyDesc* desc : cls->props) {
                    delete desc;
                }
                delete cls;
            }
            data->classes.clear();
            if (data->control != nullptr) {
                data->control->state.store(nullptr, std::memory_order_release);
                data->control->Release();
                data->control = nullptr;
            }
            // StateData 是 placement new 在 Lua userdata 里的；必须显式析构，
            // 否则 unordered_map 的桶数组不会释放（LeakSanitizer 会报）。
            data->~StateData();
        }
        return 0;
    }
};

inline char kStateDataKey;

inline StateData* GetOrCreateStateData(lua_State* L) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kStateDataKey);
    if (!lua_isnil(L, -1)) {
        auto* data = static_cast<StateData*>(lua_touserdata(L, -1));
        lua_pop(L, 1);
        BREEZE_LUA_CHECK(data != nullptr && data->control != nullptr, "breeze::lua_binding: StateData is corrupted");
        return data;
    }
    lua_pop(L, 1);

    void* raw = lua_newuserdatauv(L, sizeof(StateData), 0);
    auto* data = new (raw) StateData{new Control{}, {}};
    data->control->owner = std::this_thread::get_id();
    data->control->state.store(MainState(L), std::memory_order_release);

    lua_newtable(L);
    lua_pushcfunction(L, &StateData::Gc);
    lua_setfield(L, -2, "__gc");
    lua_setmetatable(L, -2);

    lua_pushvalue(L, -1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kStateDataKey);
    lua_pop(L, 1);
    return data;
}

}  // namespace breeze::lua_binding
