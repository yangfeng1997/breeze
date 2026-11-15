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
// userdata 布局：[ObjHeader]（共享所有权时在 header 后内联 shared_ptr<void>），对象本身用
// new 分配（所有权策略决定谁 delete）。Stack<std::shared_ptr<T>> 走实例级持有块：
// __gc 只释放 shared_ptr，不 delete 对象；Get<shared_ptr<U>> 用 aliasing ctor 支持基类调整。
// 类型身份用 kTypeTag<T> 的地址，不用字符串哈希；基类转换用类注册时生成的 static_cast。

#include <concepts>
#include <exception>
#include <functional>
#include <ostream>
#include <sstream>
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
void CopyMetamethods(lua_State* L, const ClassInfo* base, const ClassInfo* derived);

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
    new (raw) ObjHeader{kObjCookie, cls, obj, 0, 0, nullptr, nullptr};
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
    FASTLUA_CHECK(cls != nullptr, "fastlua: type is not registered");
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

// ---------- 自动元方法 ----------
// 注册类时按 C++ 能力自动挂到 metatable 上：
//   __tostring  T::ToString()（可转 std::string）或 operator<<(std::ostream&, const T&)
//   __eq        operator==
//   __lt        operator<
//   __close     T::Close() / T::close()，仅对 Lua 的 <close> 变量生效
// 只检测“类型自己定义”的能力，不做隐式转换的特殊照顾；基类元方法在 Base<B>() 时复制。
namespace detail {

template <class T>
concept HasToStringMethod = requires(const T& value) {
    { value.ToString() } -> std::convertible_to<std::string>;
};

template <class T>
concept HasStreamOutput = requires(std::ostream& stream, const T& value) { stream << value; };

template <class T>
concept HasEquality = requires(const T& left, const T& right) {
    { left == right } -> std::convertible_to<bool>;
};

template <class T>
concept HasLess = requires(const T& left, const T& right) {
    { left < right } -> std::convertible_to<bool>;
};

template <class T>
concept HasCloseMethod = requires(T& value) { value.Close(); };

template <class T>
concept HasLowerCloseMethod = requires(T& value) { value.close(); };

template <class T>
bool BuildToString(lua_State* L, ErrorSlot& err) {
    (void)err;  // -fno-exceptions 时没有 catch 分支
#if defined(__cpp_exceptions)
    try {
#endif
        T* obj = GetObjectAs<T>(L, 1, nullptr);
        if (obj == nullptr) {
            lua_pushliteral(L, "object");
            return true;
        }
        if constexpr (HasToStringMethod<T>) {
            const std::string text = obj->ToString();
            lua_pushlstring(L, text.data(), text.size());
        } else {
            std::ostringstream stream;
            stream << *obj;
            const std::string text = stream.str();
            lua_pushlstring(L, text.data(), text.size());
        }
        return true;
#if defined(__cpp_exceptions)
    } catch (const std::exception& e) {
        err.Set(ErrorCode::kRuntime, e.what());
        return false;
    } catch (...) {
        err.Set(ErrorCode::kRuntime, "unknown C++ exception in __tostring");
        return false;
    }
#endif
}

template <class T>
bool BuildEquality(lua_State* L, ErrorSlot& err) {
    (void)err;  // -fno-exceptions 时没有 catch 分支
#if defined(__cpp_exceptions)
    try {
#endif
        T* left = GetObjectAs<T>(L, 1, nullptr);
        T* right = GetObjectAs<T>(L, 2, nullptr);
        lua_pushboolean(L, (left != nullptr && right != nullptr && (*left == *right)) ? 1 : 0);
        return true;
#if defined(__cpp_exceptions)
    } catch (const std::exception& e) {
        err.Set(ErrorCode::kRuntime, e.what());
        return false;
    } catch (...) {
        err.Set(ErrorCode::kRuntime, "unknown C++ exception in __eq");
        return false;
    }
#endif
}

template <class T>
bool BuildLess(lua_State* L, ErrorSlot& err) {
#if defined(__cpp_exceptions)
    try {
#endif
        T* left = GetObjectAs<T>(L, 1, nullptr);
        T* right = GetObjectAs<T>(L, 2, nullptr);
        if (left == nullptr || right == nullptr) {
            err.Set(ErrorCode::kType, "fastlua: attempt to compare incompatible objects");
            return false;
        }
        lua_pushboolean(L, (*left < *right) ? 1 : 0);
        return true;
#if defined(__cpp_exceptions)
    } catch (const std::exception& e) {
        err.Set(ErrorCode::kRuntime, e.what());
        return false;
    } catch (...) {
        err.Set(ErrorCode::kRuntime, "unknown C++ exception in __lt");
        return false;
    }
#endif
}

template <class T>
bool BuildClose(lua_State* L, ErrorSlot& err) {
#if defined(__cpp_exceptions)
    try {
#endif
        T* obj = GetObjectAs<T>(L, 1, nullptr);
        if (obj == nullptr) {
            err.Set(ErrorCode::kType, "fastlua: __close on an invalid object");
            return false;
        }
        if constexpr (HasCloseMethod<T>) {
            obj->Close();
        } else if constexpr (HasLowerCloseMethod<T>) {
            obj->close();
        }
        return true;
#if defined(__cpp_exceptions)
    } catch (const std::exception& e) {
        err.Set(ErrorCode::kRuntime, e.what());
        return false;
    } catch (...) {
        err.Set(ErrorCode::kRuntime, "unknown C++ exception in __close");
        return false;
    }
#endif
}

}  // namespace detail

template <class T>
int ClassToString(lua_State* L) {
    ErrorSlot err{};
    if (!detail::BuildToString<T>(L, err)) {
        RaiseError(L, err);
    }
    return 1;
}

template <class T>
int ClassEqual(lua_State* L) {
    ErrorSlot err{};
    if (!detail::BuildEquality<T>(L, err)) {
        RaiseError(L, err);
    }
    return 1;
}

template <class T>
int ClassLess(lua_State* L) {
    ErrorSlot err{};
    if (!detail::BuildLess<T>(L, err)) {
        RaiseError(L, err);
    }
    return 1;
}

template <class T>
int ClassClose(lua_State* L) {
    ErrorSlot err{};
    if (!detail::BuildClose<T>(L, err)) {
        RaiseError(L, err);
    }
    return 0;
}

template <class T>
void AddAutoMetamethods(lua_State* L, int mt) {
    if constexpr (std::is_class_v<T>) {
        if constexpr (detail::HasToStringMethod<T> || detail::HasStreamOutput<T>) {
            lua_pushcfunction(L, &ClassToString<T>);
            lua_setfield(L, mt, "__tostring");
        }
        if constexpr (detail::HasEquality<T>) {
            lua_pushcfunction(L, &ClassEqual<T>);
            lua_setfield(L, mt, "__eq");
        }
        if constexpr (detail::HasLess<T>) {
            lua_pushcfunction(L, &ClassLess<T>);
            lua_setfield(L, mt, "__lt");
        }
        if constexpr (detail::HasCloseMethod<T> || detail::HasLowerCloseMethod<T>) {
            lua_pushcfunction(L, &ClassClose<T>);
            lua_setfield(L, mt, "__close");
        }
    }
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
// Lua 只接受“按 shared_ptr 推入”的对象；Class<T>().Ctor 创建的 owned 对象不能反解成
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
        CopyMetamethods(L_, base, cls_);
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

    // 显式元方法逃生门（__le / __len / __pairs / 算术等自动检测未覆盖的名字）。
    // function 是裸 lua_CFunction，自己负责取参和错误处理。
    ClassBuilder& Meta(std::string_view name, lua_CFunction function) {
        lua_rawgeti(L_, LUA_REGISTRYINDEX, cls_->mt_ref);
        lua_pushcfunction(L_, function);
        const std::string key(name);
        lua_setfield(L_, -2, key.c_str());
        lua_pop(L_, 1);
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
