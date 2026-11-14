#pragma once

// 类绑定：
//   lua.Class<Player>("Player")
//      .Ctor<int>()
//      .Method("hp", &Player::Hp)
//      .Prop("name", &Player::Name, &Player::SetName)
//      .Base<Entity>()
//      .Owner(Policy::kOwned)
//      .AllowFields()
//      .DotCall();
//
// userdata 布局：[ObjHeader][外部对象指针]，对象本身用 new 分配（所有权策略决定谁 delete）。
// 类型身份用 kTypeTag<T> 的地址，不用字符串哈希；基类转换用类注册时生成的 static_cast。

#include <functional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "base/fastlua/check.h"
#include "base/fastlua/class_info.h"
#include "base/fastlua/config.h"
#include "base/fastlua/error.h"
#include "base/fastlua/lua_state.h"
#include "base/fastlua/ref.h"
#include "base/fastlua/stack.h"
#include "base/fastlua/trampoline.h"

namespace breeze::fastlua {

int ClassGc(lua_State* L);
int ClassIndex(lua_State* L);
int ClassNewIndex(lua_State* L);
int DotThunk(lua_State* L);
void CopyMissing(lua_State* L, int from_ref, int to_ref);
void CopyProps(lua_State* L, const ClassInfo* base, ClassInfo* derived, void* (*cast)(void*));

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
    FASTLUA_CHECK(cls != nullptr, "fastlua: type is not registered");
    if (obj == nullptr) {
        lua_pushnil(L);
        return;
    }
    void* raw = lua_newuserdatauv(L, sizeof(ObjHeader), cls->allow_fields ? 1 : 0);
    new (raw) ObjHeader{kObjCookie, cls, obj, 0, 0};
    SetObjectMetatable(L, cls);
}

template <class T>
void PushOwnedObject(lua_State* L, T* obj, ClassInfo* cls) {
    FASTLUA_CHECK(cls != nullptr, "fastlua: type is not registered");
    if (obj == nullptr) {
        lua_pushnil(L);
        return;
    }
    void* raw = lua_newuserdatauv(L, sizeof(ObjHeader), cls->allow_fields ? 1 : 0);
    new (raw) ObjHeader{kObjCookie, cls, obj, kObjOwned, 0};
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

// ---------- 成员函数调用 ----------
template <class M>
int TrampMember(lua_State* L);

template <class M>
[[gnu::noinline]] int InvokeMember(lua_State* L, M member, const ClassInfo* declaring, ErrorSlot& err) {
    using traits = function_traits<M>;
    using C = typename traits::class_type;
    using args_tuple = typename traits::method_args;
    constexpr std::size_t kArity = std::tuple_size_v<args_tuple>;

    C* self = GetObjectAs<C>(L, 1, declaring);
    if (self == nullptr) {
        err.Set(ErrorCode::kType, "fastlua: bad self (expected object)");
        return -1;
    }
    if (!CheckArgsAt(L, err, 1, static_cast<args_tuple*>(nullptr), std::make_index_sequence<kArity>{})) {
        return -1;
    }
    auto args = BuildArgsAt(L, 1, static_cast<args_tuple*>(nullptr), std::make_index_sequence<kArity>{});
    auto full = std::tuple_cat(std::tuple<C*>{self}, std::move(args));
    using R = typename traits::return_type;

#if defined(__cpp_exceptions)
    try {
#endif
        if constexpr (std::is_void_v<R>) {
            std::apply(member, full);
            return 0;
        } else {
            R result = std::apply(member, full);
            return PushReturn(L, std::move(result));
        }
#if defined(__cpp_exceptions)
    } catch (const std::exception& e) {
        err.Set(ErrorCode::kRuntime, e.what());
        return -1;
    } catch (...) {
        err.Set(ErrorCode::kRuntime, "unknown C++ exception in a bound method");
        return -1;
    }
#endif
}

template <class M>
int TrampolineMember(lua_State* L, M member, const ClassInfo* declaring) {
    ErrorSlot err{};
    const int result = InvokeMember<M>(L, member, declaring, err);
    if (result >= 0) {
        return result;
    }
    RaiseError(L, err);
}

template <class M>
int TrampMember(lua_State* L) {
    auto* header = static_cast<CallableHeader*>(lua_touserdata(L, lua_upvalueindex(1)));
    FASTLUA_CHECK(header != nullptr && header->destroy != nullptr, "fastlua: member upvalue is invalid");
    auto* member = reinterpret_cast<M*>(header + 1);
    auto* declaring = static_cast<const ClassInfo*>(lua_touserdata(L, lua_upvalueindex(2)));
    return TrampolineMember<M>(L, *member, declaring);
}

// ---------- 构造函数 ----------
template <class T, class... A>
[[gnu::noinline]] int InvokeCtor(lua_State* L, ClassInfo* cls, ErrorSlot& err) {
    using args_tuple = std::tuple<A...>;
    constexpr std::size_t kArity = sizeof...(A);
    if (!CheckArgsAt(L, err, 1, static_cast<args_tuple*>(nullptr), std::make_index_sequence<kArity>{})) {
        return -1;
    }
    auto args = BuildArgsAt(L, 1, static_cast<args_tuple*>(nullptr), std::make_index_sequence<kArity>{});

    T* obj = nullptr;
#if defined(__cpp_exceptions)
    try {
#endif
        obj = std::apply([](auto&&... item) { return new T(std::forward<decltype(item)>(item)...); }, args);
#if defined(__cpp_exceptions)
    } catch (const std::exception& e) {
        err.Set(ErrorCode::kRuntime, e.what());
        return -1;
    } catch (...) {
        err.Set(ErrorCode::kRuntime, "unknown C++ exception in a bound constructor");
        return -1;
    }
#endif
    if (obj == nullptr) {
        err.Set(ErrorCode::kRuntime, "fastlua: constructor returned null");
        return -1;
    }
    PushOwnedObject<T>(L, obj, cls);
    return 1;
}

template <class T, class... A>
int TrampCtor(lua_State* L) {
    auto* cls = static_cast<ClassInfo*>(lua_touserdata(L, lua_upvalueindex(1)));
    ErrorSlot err{};
    const int result = InvokeCtor<T, A...>(L, cls, err);
    if (result >= 0) {
        return result;
    }
    RaiseError(L, err);
}

// ---------- 建类 ----------
template <class T>
ClassInfo* EnsureClass(lua_State* L, std::string_view name) {
    StateData* data = GetOrCreateStateData(L);
    if (const auto it = data->classes.find(&kTypeTag<T>); it != data->classes.end()) {
        return it->second;
    }

    auto* cls = new ClassInfo;
    cls->name = std::string(name);
    cls->type_tag = &kTypeTag<T>;
    cls->destroy = [](ObjHeader* header) { delete static_cast<T*>(header->obj); };
    cls->policy = Policy::kOwned;

    lua_newtable(L);
    cls->methods_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_newtable(L);
    cls->getters_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_newtable(L);
    cls->setters_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    lua_newtable(L);
    const int mt = lua_gettop(L);
    lua_pushlstring(L, name.data(), name.size());
    lua_setfield(L, mt, "__name");
    lua_pushcfunction(L, &ClassGc);
    lua_setfield(L, mt, "__gc");

    lua_rawgeti(L, LUA_REGISTRYINDEX, cls->methods_ref);
    lua_rawgeti(L, LUA_REGISTRYINDEX, cls->getters_ref);
    lua_pushlightuserdata(L, cls);
    lua_pushcclosure(L, &ClassIndex, 3);
    lua_setfield(L, mt, "__index");

    lua_rawgeti(L, LUA_REGISTRYINDEX, cls->setters_ref);
    lua_pushlightuserdata(L, cls);
    lua_pushcclosure(L, &ClassNewIndex, 2);
    lua_setfield(L, mt, "__newindex");

    cls->mt_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    lua_newtable(L);
    cls->class_meta_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    lua_newtable(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, cls->class_meta_ref);
    lua_setmetatable(L, -2);
    lua_pushvalue(L, -1);
    lua_setglobal(L, cls->name.c_str());
    lua_pop(L, 1);

    data->classes.emplace(cls->type_tag, cls);
    return cls;
}

template <class T>
class ClassBuilder {
public:
    ClassBuilder(lua_State* L, std::string_view name) : L_(L), cls_(EnsureClass<T>(L, name)) {}

    template <class... A>
    ClassBuilder& Ctor() {
        lua_rawgeti(L_, LUA_REGISTRYINDEX, cls_->class_meta_ref);
        lua_pushlightuserdata(L_, cls_);
        lua_pushcclosure(L_, &TrampCtor<T, A...>, 1);
        lua_setfield(L_, -2, "__call");
        lua_pop(L_, 1);
        return *this;
    }

    template <class M>
    ClassBuilder& Method(std::string_view name, M member) {
        lua_rawgeti(L_, LUA_REGISTRYINDEX, cls_->methods_ref);
        void* raw = lua_newuserdatauv(L_, sizeof(CallableHeader) + sizeof(M), 0);
        auto* header = new (raw) CallableHeader{&DestroyCallable<M>};
        new (static_cast<void*>(header + 1)) M(member);
        lua_pushlightuserdata(L_, cls_);
        lua_pushcclosure(L_, &TrampMember<M>, 2);
        const std::string key(name);
        lua_setfield(L_, -2, key.c_str());
        lua_pop(L_, 1);
        return *this;
    }

    // 键值糖衣用：成员函数指针 -> Method，数据成员指针 -> Prop。
    template <class M>
    ClassBuilder& Add(std::string_view name, M member) {
        if constexpr (std::is_member_function_pointer_v<M>) {
            return Method(name, member);
        } else {
            return Prop(name, member);
        }
    }

    template <class G, class S>
    ClassBuilder& Prop(std::string_view name, G getter, S setter) {
        using GT = function_traits<G>;
        using SC = typename function_traits<S>::class_type;
        using SetArg = std::tuple_element_t<0, typename function_traits<S>::method_args>;

        auto* desc = new PropDesc;
        desc->getter = [getter](lua_State* L, void* obj) {
            auto&& value = std::invoke(getter, static_cast<typename GT::class_type*>(obj));
            using R = std::remove_cvref_t<decltype(value)>;
            Stack<R>::Push(L, value);
        };
        desc->setter = [setter](lua_State* L, void* obj) {
            ArgStorage<SetArg> value = Stack<ArgStorage<SetArg>>::Get(L, 3);
            std::invoke(setter, static_cast<SC*>(obj), std::move(value));
        };
        cls_->props.push_back(desc);
        AddProp(name, desc);
        return *this;
    }

    template <class M>
    ClassBuilder& Prop(std::string_view name, M T::*member) {
        auto* desc = new PropDesc;
        desc->getter = [member](lua_State* L, void* obj) {
            Stack<M>::Push(L, static_cast<T*>(obj)->*member);
        };
        desc->setter = [member](lua_State* L, void* obj) {
            static_cast<T*>(obj)->*member = Stack<M>::Get(L, 3);
        };
        cls_->props.push_back(desc);
        AddProp(name, desc);
        return *this;
    }

    template <class G>
    ClassBuilder& PropReadOnly(std::string_view name, G getter) {
        using GT = function_traits<G>;
        auto* desc = new PropDesc;
        desc->getter = [getter](lua_State* L, void* obj) {
            auto&& value = std::invoke(getter, static_cast<typename GT::class_type*>(obj));
            using R = std::remove_cvref_t<decltype(value)>;
            Stack<R>::Push(L, value);
        };
        cls_->props.push_back(desc);
        AddProp(name, desc);
        return *this;
    }

    template <class B>
    ClassBuilder& Base() {
        ClassInfo* base = GetClass<B>(L_);
        FASTLUA_CHECK(base != nullptr, "fastlua: base class must be registered before being used as a base");
        void* (*up)(void*) = [](void* p) -> void* { return static_cast<B*>(static_cast<T*>(p)); };
        cls_->ancestors.push_back(ClassInfo::Base{base, up});
        CopyMissing(L_, base->methods_ref, cls_->methods_ref);
        CopyProps(L_, base, cls_, up);
        return *this;
    }

    ClassBuilder& Owner(Policy policy) {
        cls_->policy = policy;
        if (policy == Policy::kOwned) {
            cls_->destroy = [](ObjHeader* header) { delete static_cast<T*>(header->obj); };
        } else {
            cls_->destroy = nullptr;
        }
        return *this;
    }

    ClassBuilder& AllowFields() {
        cls_->allow_fields = true;
        return *this;
    }

    ClassBuilder& DotCall() {
        cls_->dot_call = true;
        return *this;
    }

private:
    void AddProp(std::string_view name, PropDesc* desc) {
        const std::string key(name);
        lua_rawgeti(L_, LUA_REGISTRYINDEX, cls_->getters_ref);
        lua_pushlightuserdata(L_, desc);
        lua_setfield(L_, -2, key.c_str());
        lua_pop(L_, 1);

        lua_rawgeti(L_, LUA_REGISTRYINDEX, cls_->setters_ref);
        lua_pushlightuserdata(L_, desc);
        lua_setfield(L_, -2, key.c_str());
        lua_pop(L_, 1);
    }

    lua_State* L_ = nullptr;
    ClassInfo* cls_ = nullptr;
};

template <class T>
ClassBuilder<T> LuaStateView::Class(std::string_view name) {
    return ClassBuilder<T>(L_, name);
}

}  // namespace breeze::fastlua
