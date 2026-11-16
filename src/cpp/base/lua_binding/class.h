#pragma once

// 类绑定：
//   lua.Class<Player>("Player")
//      .Constructor<int>()
//      .Method("hp", &Player::Hp)
//      .Property("name", &Player::Name, &Player::SetName)
//      .Base<Entity>()
//      .Owner(Policy::kOwned)
//      .AllowFields()
//      .DotCall();
//
// userdata 布局：[ObjHeader]（共享所有权时在 header 后内联 shared_ptr<void>），对象本身用
// new 分配（所有权策略决定谁 delete）。Stack<std::shared_ptr<T>> 走实例级持有块：
// __gc 只释放 shared_ptr，不 delete 对象；Get<shared_ptr<U>> 用 aliasing ctor 支持基类调整。
// 类型身份用 kTypeTag<T> 的地址，不用字符串哈希；基类转换用类注册时生成的 static_cast。

#include <functional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "base/lua_binding/check.h"
#include "base/lua_binding/class_info.h"
#include "base/lua_binding/config.h"
#include "base/lua_binding/error.h"
#include "base/lua_binding/lua_state.h"
#include "base/lua_binding/metamethod.h"
#include "base/lua_binding/object.h"
#include "base/lua_binding/stack.h"
#include "base/lua_binding/trampoline.h"

namespace breeze::lua_binding {

int ClassGc(lua_State* L);
int ClassIndex(lua_State* L);
int ClassNewIndex(lua_State* L);
int DotThunk(lua_State* L);
void CopyMissing(lua_State* L, int from_ref, int to_ref);
void CopyProps(lua_State* L, const ClassInfo* base, ClassInfo* derived, void* (*cast)(void*));
void CopyMetamethods(lua_State* L, const ClassInfo* base, const ClassInfo* derived);

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
        err.Set(ErrorCode::kType, "breeze::lua_binding: bad self (expected object)");
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
    BREEZE_LUA_CHECK(header != nullptr && header->destroy != nullptr, "breeze::lua_binding: member upvalue is invalid");
    auto* member = reinterpret_cast<M*>(header + 1);
    auto* declaring = static_cast<const ClassInfo*>(lua_touserdata(L, lua_upvalueindex(2)));
    return TrampolineMember<M>(L, *member, declaring);
}

// ---------- 构造函数 ----------
template <class T, class... A>
[[gnu::noinline]] int InvokeConstructor(lua_State* L, ClassInfo* cls, ErrorSlot& err) {
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
        err.Set(ErrorCode::kRuntime, "breeze::lua_binding: constructor returned null");
        return -1;
    }
    PushOwnedObject<T>(L, obj, cls);
    return 1;
}

template <class T, class... A>
int TrampConstructor(lua_State* L) {
    auto* cls = static_cast<ClassInfo*>(lua_touserdata(L, lua_upvalueindex(1)));
    ErrorSlot err{};
    const int result = InvokeConstructor<T, A...>(L, cls, err);
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

    AddAutoMetamethods<T>(L, mt);

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
    ClassBuilder& Constructor() {
        lua_rawgeti(L_, LUA_REGISTRYINDEX, cls_->class_meta_ref);
        lua_pushlightuserdata(L_, cls_);
        lua_pushcclosure(L_, &TrampConstructor<T, A...>, 1);
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

    // 键值糖衣用：成员函数指针 -> Method，数据成员指针 -> Property。
    template <class M>
    ClassBuilder& Add(std::string_view name, M member) {
        if constexpr (std::is_member_function_pointer_v<M>) {
            return Method(name, member);
        } else {
            return Property(name, member);
        }
    }

    template <class G, class S>
    ClassBuilder& Property(std::string_view name, G getter, S setter) {
        using GT = function_traits<G>;
        using SC = typename function_traits<S>::class_type;
        using SetArg = std::tuple_element_t<0, typename function_traits<S>::method_args>;

        auto* desc = new PropertyDesc;
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
    ClassBuilder& Property(std::string_view name, M T::*member) {
        auto* desc = new PropertyDesc;
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
    ClassBuilder& PropertyReadOnly(std::string_view name, G getter) {
        using GT = function_traits<G>;
        auto* desc = new PropertyDesc;
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
        BREEZE_LUA_CHECK(base != nullptr, "breeze::lua_binding: base class must be registered before being used as a base");
        void* (*up)(void*) = [](void* p) -> void* { return static_cast<B*>(static_cast<T*>(p)); };
        cls_->ancestors.push_back(ClassInfo::Base{base, up});
        CopyMissing(L_, base->methods_ref, cls_->methods_ref);
        CopyProps(L_, base, cls_, up);
        CopyMetamethods(L_, base, cls_);
        return *this;
    }

    ClassBuilder& Owner(Policy policy) {
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

    // 显式元方法逃生门（__le / __len / __pairs / 算术等自动检测未覆盖的名字）。
    // function 是裸 lua_CFunction，自己负责取参和错误处理。
    ClassBuilder& MetaMethod(std::string_view name, lua_CFunction function) {
        lua_rawgeti(L_, LUA_REGISTRYINDEX, cls_->mt_ref);
        lua_pushcfunction(L_, function);
        const std::string key(name);
        lua_setfield(L_, -2, key.c_str());
        lua_pop(L_, 1);
        return *this;
    }

private:
    void AddProp(std::string_view name, PropertyDesc* desc) {
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

}  // namespace breeze::lua_binding
