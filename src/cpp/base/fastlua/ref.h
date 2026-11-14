#pragma once

// 值类型：
//   TableView / FunctionView  栈上的视图，只在当前 C 函数内有效，不占 registry。
//   Ref / Table / Function     registry 引用，绑定主线程；lua_close 之后析构是空操作。
//
// Control 是引用计数块：StateData 的 __gc 在 lua_close 时把 state 置空，
// 所以 lua_close 之后再析构 Ref 不会碰已经释放的 lua_State。
//
// 线程规则（必须遵守）：Ref / Table / Function 必须在创建它的线程（owner）上析构。
// 异步 worker / IO / DB 线程只传 ID 或令牌，不传 Ref；任意线程可析构的 C++ 句柄
// （如 ReplyHandle）不得直接持有 Ref。违反是用法错误：这里不碰 VM，只记录一次并
// 泄漏该 registry 槽。库刻意不实现跨线程释放队列——sol2、LuaBridge3、luakit 同样不做。

#include <atomic>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "base/fastlua/call.h"
#include "base/fastlua/check.h"
#include "base/fastlua/class_info.h"
#include "base/fastlua/config.h"
#include "base/fastlua/error.h"
#include "base/fastlua/stack.h"

namespace breeze::fastlua {

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
                for (PropDesc* desc : cls->props) {
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
        FASTLUA_CHECK(data != nullptr && data->control != nullptr, "fastlua: StateData is corrupted");
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

class Ref {
public:
    Ref() noexcept = default;
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;
    Ref(Ref&& other) noexcept : control_(other.control_), ref_(other.ref_) {
        other.control_ = nullptr;
        other.ref_ = LUA_NOREF;
    }
    Ref& operator=(Ref&& other) noexcept {
        if (this != &other) {
            Reset();
            control_ = other.control_;
            ref_ = other.ref_;
            other.control_ = nullptr;
            other.ref_ = LUA_NOREF;
        }
        return *this;
    }
    ~Ref() { Reset(); }

    bool Valid() const noexcept { return control_ != nullptr && State() != nullptr; }

    int Push() const {
        lua_State* L = State();
        FASTLUA_CHECK(L != nullptr, "fastlua: Ref used after its LuaState was closed");
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref_);
        return lua_gettop(L);
    }

    static Ref FromStack(lua_State* L, int index) {
        index = lua_absindex(L, index);
        lua_pushvalue(L, index);
        StateData* data = GetOrCreateStateData(L);
        data->control->AddRef();
        Ref out;
        out.control_ = data->control;
        out.ref_ = luaL_ref(L, LUA_REGISTRYINDEX);
        return out;
    }

protected:
    lua_State* State() const noexcept {
        return control_ != nullptr ? control_->state.load(std::memory_order_acquire) : nullptr;
    }

    void Reset() noexcept {
        if (control_ != nullptr) {
            lua_State* L = control_->state.load(std::memory_order_acquire);
            if (L != nullptr && ref_ != LUA_NOREF && ref_ != LUA_REFNIL) {
                if (std::this_thread::get_id() == control_->owner) {
                    luaL_unref(L, LUA_REGISTRYINDEX, ref_);
                } else {
                    // 用法错误：Ref 必须在 owner 线程析构。不碰 VM，只记录一次并泄漏槽。
                    FASTLUA_ENSURE(false,
                                   "fastlua: Ref destroyed on a non-owner thread; leaking registry slot "
                                   "(pass IDs across threads, not Ref)");
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

class Table : public Ref {
public:
    Table() noexcept = default;

    static Table FromStack(lua_State* L, int index) {
        FASTLUA_CHECK(lua_istable(L, index), "fastlua: Table::FromStack expects a table");
        Table out;
        static_cast<Ref&>(out) = Ref::FromStack(L, index);
        return out;
    }

    template <class T>
    Result<T> Get(std::string_view key) const {
        lua_State* L = State();
        if (L == nullptr) {
            return Error{ErrorCode::kRuntime, "fastlua: Table used after its LuaState was closed"};
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
            return Error{ErrorCode::kNotFound, "fastlua: key '" + std::string(key) + "' not found"};
        }
        if (!Stack<T>::Check(L, -1)) {
            const std::string actual = luaL_typename(L, -1);
            lua_settop(L, top);
            return Error{ErrorCode::kType, "fastlua: key '" + std::string(key) + "' expects " + Stack<T>::kName +
                                               ", got " + actual};
        }
        T value = Stack<T>::Get(L, -1);
        lua_settop(L, top);
        return value;
    }

    template <class T>
    void Set(std::string_view key, T&& value) const {
        lua_State* L = State();
        FASTLUA_CHECK(L != nullptr, "fastlua: Table used after its LuaState was closed");
        const int top = lua_gettop(L);
        Push();
        lua_pushlstring(L, key.data(), key.size());
        Stack<std::decay_t<T>>::Push(L, std::forward<T>(value));
        lua_settable(L, top + 1);
        lua_settop(L, top);
    }

    template <class R, class... Args>
    Result<R> Call(std::string_view function, Args&&... args) const {
        lua_State* L = State();
        if (L == nullptr) {
            return Error{ErrorCode::kRuntime, "fastlua: Table used after its LuaState was closed"};
        }
        const int top = lua_gettop(L);
        Push();
        lua_getfield(L, top + 1, std::string(function).c_str());
        if (!lua_isfunction(L, -1)) {
            lua_settop(L, top);
            return Error{ErrorCode::kNotFound, "fastlua: table function '" + std::string(function) + "' not found"};
        }
        Result<R> result = CallPushed<R>(L, lua_gettop(L), std::forward<Args>(args)...);
        lua_settop(L, top);
        return result;
    }
};

class Function : public Ref {
public:
    Function() noexcept = default;

    static Function FromStack(lua_State* L, int index) {
        FASTLUA_CHECK(lua_isfunction(L, index), "fastlua: Function::FromStack expects a function");
        Function out;
        static_cast<Ref&>(out) = Ref::FromStack(L, index);
        return out;
    }

    template <class R, class... Args>
    Result<R> Call(Args&&... args) const {
        lua_State* L = State();
        if (L == nullptr) {
            return Error{ErrorCode::kRuntime, "fastlua: Function used after its LuaState was closed"};
        }
        Push();
        return CallPushed<R>(L, lua_gettop(L), std::forward<Args>(args)...);
    }

    // 热路径：不挂 traceback。错误仍然由 pcall 捕获并返回 Result。
    template <class R, class... Args>
    Result<R> CallFast(Args&&... args) const {
        lua_State* L = State();
        if (L == nullptr) {
            return Error{ErrorCode::kRuntime, "fastlua: Function used after its LuaState was closed"};
        }
        Push();
        return CallPushedFast<R>(L, lua_gettop(L), std::forward<Args>(args)...);
    }
};

// 栈视图：只在当前 C 函数内有效。
struct TableView {
    lua_State* L = nullptr;
    int index = 0;

    // 热路径：key 是字面量时用 lua_getfield，避免每次 push 一个 Lua 字符串；
    // 返回 bool 而不是 Result，不构造错误消息。
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
    Result<T> Get(std::string_view key) const {
        const int abs = lua_absindex(L, index);
        const int top = lua_gettop(L);
        lua_pushlstring(L, key.data(), key.size());
        lua_gettable(L, abs);
        if (lua_isnil(L, -1) && !Stack<T>::Check(L, -1)) {
            lua_settop(L, top);
            return Error{ErrorCode::kNotFound, "fastlua: key '" + std::string(key) + "' not found"};
        }
        if (!Stack<T>::Check(L, -1)) {
            const std::string actual = luaL_typename(L, -1);
            lua_settop(L, top);
            return Error{ErrorCode::kType, "fastlua: key '" + std::string(key) + "' expects " + Stack<T>::kName +
                                               ", got " + actual};
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
struct Stack<TableView, void> {
    static constexpr const char* kName = "table";

    static bool Check(lua_State* L, int index) { return lua_istable(L, index); }
    static TableView Get(lua_State* L, int index) { return TableView{L, lua_absindex(L, index)}; }
};

struct FunctionView {
    lua_State* L = nullptr;
    int index = 0;

    template <class R, class... Args>
    Result<R> Call(Args&&... args) const {
        const int abs = lua_absindex(L, index);
        lua_pushvalue(L, abs);
        return CallPushed<R>(L, lua_gettop(L), std::forward<Args>(args)...);
    }
};

}  // namespace breeze::fastlua
