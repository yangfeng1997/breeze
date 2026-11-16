#pragma once

// C++ 对象 ↔ Lua userdata：类型查找、所有权推送、Stack<T*> / T& / shared_ptr。
// 类注册器在 class.h；这里只处理对象本身。

#include <memory>
#include <type_traits>

#include "base/lua_binding/check.h"
#include "base/lua_binding/class_info.h"
#include "base/lua_binding/config.h"
#include "base/lua_binding/stack.h"
#include "base/lua_binding/state_data.h"

namespace breeze::lua_binding {

inline ObjHeader* GetHeader(lua_State* L, int index) {
    if (lua_type(L, index) != LUA_TUSERDATA) {
        return nullptr;
    }
    if (lua_rawlen(L, index) < sizeof(ObjHeader)) {
        return nullptr;
    }
    auto* header = static_cast<ObjHeader*>(lua_touserdata(L, index));
    if (header == nullptr || header->cookie != kObjCookie || header->obj == nullptr) {
        return nullptr;
    }
    return header;
}

template <class T>
ClassInfo* GetClass(lua_State* L) {
    StateData* data = GetOrCreateStateData(L);
    const auto it = data->classes.find(&kTypeTag<T>);
    return it == data->classes.end() ? nullptr : it->second;
}

template <class T>
T* GetObjectAs(lua_State* L, int index, const ClassInfo* declaring) {
    ObjHeader* header = GetHeader(L, index);
    if (header == nullptr) {
        return nullptr;
    }
    if (declaring == nullptr) {
        declaring = GetClass<T>(L);
    }
    if (declaring == nullptr) {
        return nullptr;
    }
    if (header->cls->type_tag == declaring->type_tag) {
        return static_cast<T*>(header->obj);
    }
    for (const auto& ancestor : header->cls->ancestors) {
        if (ancestor.info->type_tag == declaring->type_tag) {
            return static_cast<T*>(ancestor.up(header->obj));
        }
    }
    return nullptr;
}

inline void SetObjectMetatable(lua_State* L, const ClassInfo* cls) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, cls->mt_ref);
    lua_setmetatable(L, -2);
}

template <class T>
void PushBorrowedObject(lua_State* L, T* obj, ClassInfo* cls) {
    BREEZE_LUA_CHECK(cls != nullptr, "breeze::lua_binding: type is not registered");
    if (obj == nullptr) {
        lua_pushnil(L);
        return;
    }
    void* raw = lua_newuserdatauv(L, sizeof(ObjHeader), cls->allow_fields ? 1 : 0);
    new (raw) ObjHeader{kObjCookie, cls, obj, 0, 0, nullptr, nullptr};
    SetObjectMetatable(L, cls);
}

template <class T>
void PushOwnedObject(lua_State* L, T* obj, ClassInfo* cls) {
    BREEZE_LUA_CHECK(cls != nullptr, "breeze::lua_binding: type is not registered");
    if (obj == nullptr) {
        lua_pushnil(L);
        return;
    }
    void* raw = lua_newuserdatauv(L, sizeof(ObjHeader), cls->allow_fields ? 1 : 0);
    new (raw) ObjHeader{kObjCookie, cls, obj, kObjOwned, 0, nullptr, nullptr};
    SetObjectMetatable(L, cls);
}

// shared_ptr 持有块直接放在 userdata 的 ObjHeader 之后（不再单独 new）：
// owner 保留控制块，obj 指向具体的 T（可能是基类子对象）；Get<shared_ptr<U>>
// 用 aliasing 构造函数从 owner + GetObjectAs<U> 得到正确的 U 指针。
inline void DestroySharedHolder(void* holder) {
    static_cast<std::shared_ptr<void>*>(holder)->~shared_ptr();
}

template <class T>
void PushSharedObject(lua_State* L, std::shared_ptr<T> obj, ClassInfo* cls) {
    BREEZE_LUA_CHECK(cls != nullptr, "breeze::lua_binding: type is not registered");
    if (!obj) {
        lua_pushnil(L);
        return;
    }
    void* raw = lua_newuserdatauv(L, sizeof(ObjHeader) + sizeof(std::shared_ptr<void>),
                                  cls->allow_fields ? 1 : 0);
    void* holder = static_cast<char*>(raw) + sizeof(ObjHeader);
    new (holder) std::shared_ptr<void>(obj);
    new (raw) ObjHeader{kObjCookie, cls, obj.get(), kObjShared, 0, holder, &DestroySharedHolder};
    SetObjectMetatable(L, cls);
}


// ---------- 对象指针转换 ----------
template <class T>
struct Stack<T*, void> {
    static constexpr const char* kName = "object";

    static bool Check(lua_State* L, int index) { return GetObjectAs<T>(L, index, nullptr) != nullptr; }
    static T* Get(lua_State* L, int index) { return GetObjectAs<T>(L, index, nullptr); }
    static void Push(lua_State* L, T* obj) { PushBorrowedObject<T>(L, obj, GetClass<T>(L)); }
};

template <class T>
struct Stack<T&, void> {
    static constexpr const char* kName = "object";

    static bool Check(lua_State* L, int index) { return GetObjectAs<T>(L, index, nullptr) != nullptr; }
    static T& Get(lua_State* L, int index) { return *GetObjectAs<T>(L, index, nullptr); }
    static void Push(lua_State* L, T& obj) { PushBorrowedObject<T>(L, &obj, GetClass<T>(L)); }
};

// ---------- shared_ptr ----------
// Lua 只接受“按 shared_ptr 推入”的对象；Class<T>().Constructor 创建的 owned 对象不能反解成
// shared_ptr（没有控制块，除非将来支持 enable_shared_from_this）。
// 不绑定 weak_ptr：Lua 侧做不了安全的非拥有句柄，C++ 侧需要时请自行用 std::weak_ptr。
template <class T>
struct Stack<std::shared_ptr<T>, void> {
    static constexpr const char* kName = "object";

    static bool Check(lua_State* L, int index) {
        if (lua_type(L, index) == LUA_TNIL) {
            return true;
        }
        const ObjHeader* header = GetHeader(L, index);
        return header != nullptr && (header->flags & kObjShared) != 0 &&
               GetObjectAs<T>(L, index, nullptr) != nullptr;
    }

    static std::shared_ptr<T> Get(lua_State* L, int index) {
        if (lua_type(L, index) == LUA_TNIL) {
            return {};
        }
        ObjHeader* header = GetHeader(L, index);
        T* obj = GetObjectAs<T>(L, index, nullptr);
        if (header == nullptr || obj == nullptr || (header->flags & kObjShared) == 0 || header->holder == nullptr) {
            return {};
        }
        auto* owner = static_cast<std::shared_ptr<void>*>(header->holder);
        return std::shared_ptr<T>(*owner, obj);
    }

    static void Push(lua_State* L, std::shared_ptr<T> obj) {
        if (!obj) {
            lua_pushnil(L);
            return;
        }
        PushSharedObject<T>(L, std::move(obj), GetClass<T>(L));
    }
};

}  // namespace breeze::lua_binding
