#pragma once

// LuaState：拥有 lua_State 的入口；LuaStateView：非拥有视图，用于 luaopen_* 与外部状态。
// 一个 LuaState 绑定一个线程（一个 Shard）。跨线程使用 lua_State 是 Lua 禁止的。

#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "base/fastlua/call.h"
#include "base/fastlua/ref.h"
#include "base/fastlua/check.h"
#include "base/fastlua/config.h"
#include "base/fastlua/error.h"
#include "base/fastlua/stack.h"
#include "base/fastlua/trampoline.h"

namespace breeze::fastlua {

// 全局可调用对象 metatable 的 registry key（取地址当 key，不用字符串）。
inline char kCallableMetaKey;

template <class D, class = void>
struct IsCallable : std::false_type {};
template <class D>
struct IsCallable<D, std::enable_if_t<std::is_pointer_v<D> && std::is_function_v<std::remove_pointer_t<D>>>>
    : std::true_type {};
template <class D>
struct IsCallable<D, std::void_t<decltype(&D::operator())>> : std::true_type {};

template <class T>
class ClassBuilder;

struct LuaStateOptions {
    lua_Alloc allocator = nullptr;   // 空则用 Lua 默认分配器
    void* allocator_ud = nullptr;
    unsigned libs = ~0U;             // 自定义分配器时用 luaL_openselectedlibs 的位掩码
};

class LuaStateView {
public:
    LuaStateView() noexcept = default;
    explicit LuaStateView(lua_State* L) noexcept : L_(L) {}

    lua_State* Raw() const noexcept { return L_; }

    // 值是数据：走 Stack<T>::Push；值是可调用对象：走 SetFunction。
    template <class T>
    void Set(std::string_view name, T&& value) {
        using D = std::decay_t<T>;
        if constexpr (IsCallable<D>::value) {
            SetFunction(name, std::forward<T>(value));
        } else {
            static_assert(!is_tuple_like_v<D>, "fastlua: Set does not accept tuple/pair as a value");
            Stack<D>::Push(L_, std::forward<T>(value));
            const std::string key(name);
            lua_setglobal(L_, key.c_str());
        }
    }

    // 只返回拥有所有权的类型；string_view / const char* 会悬空，禁止。
    template <class T>
    Result<T> Get(std::string_view name) const {
        static_assert(!std::is_same_v<T, std::string_view> && !std::is_same_v<T, const char*>,
                      "fastlua: Get returns owning values; use std::string");
        const std::string key(name);
        lua_getglobal(L_, key.c_str());
        if (lua_isnil(L_, -1)) {
            // optional / nullptr_t 接受 nil；其它类型把 nil 当成“不存在”。
            if (Stack<T>::Check(L_, -1)) {
                T value = Stack<T>::Get(L_, -1);
                lua_pop(L_, 1);
                return value;
            }
            lua_pop(L_, 1);
            return Error{ErrorCode::kNotFound, "fastlua: global '" + key + "' not found"};
        }
        if (!Stack<T>::Check(L_, -1)) {
            const std::string actual = luaL_typename(L_, -1);
            lua_pop(L_, 1);
            return Error{ErrorCode::kType, "fastlua: global '" + key + "' expects " + Stack<T>::kName +
                                               ", got " + actual};
        }
        T value = Stack<T>::Get(L_, -1);
        lua_pop(L_, 1);
        return value;
    }

    Result<Table> GetTable(std::string_view name) const {
        const std::string key(name);
        lua_getglobal(L_, key.c_str());
        if (lua_isnil(L_, -1)) {
            lua_pop(L_, 1);
            return Error{ErrorCode::kNotFound, "fastlua: global '" + key + "' not found"};
        }
        if (!lua_istable(L_, -1)) {
            const std::string actual = luaL_typename(L_, -1);
            lua_pop(L_, 1);
            return Error{ErrorCode::kType, "fastlua: global '" + key + "' expects table, got " + actual};
        }
        Table table = Table::FromStack(L_, -1);
        lua_pop(L_, 1);
        return table;
    }

    Result<Function> GetFunction(std::string_view name) const {
        const std::string key(name);
        lua_getglobal(L_, key.c_str());
        if (lua_isnil(L_, -1)) {
            lua_pop(L_, 1);
            return Error{ErrorCode::kNotFound, "fastlua: global '" + key + "' not found"};
        }
        if (!lua_isfunction(L_, -1)) {
            const std::string actual = luaL_typename(L_, -1);
            lua_pop(L_, 1);
            return Error{ErrorCode::kType, "fastlua: global '" + key + "' expects function, got " + actual};
        }
        Function function = Function::FromStack(L_, -1);
        lua_pop(L_, 1);
        return function;
    }

    // 最热路径的逃生门：直接挂裸 lua_CFunction，不经过 trampoline / 参数检查。
    // 用错类型是 Lua 侧的 UB；只建议给已经确认过参数、每帧调用量极大的 handler。
    void SetRawFunction(std::string_view name, lua_CFunction function) {
        lua_pushcfunction(L_, function);
        const std::string key(name);
        lua_setglobal(L_, key.c_str());
    }

    template <class F>
    void SetFunction(std::string_view name, F&& fn) {
        using D = std::decay_t<F>;
        EnsureCallableMeta();
        void* raw = lua_newuserdatauv(L_, sizeof(CallableHeader) + sizeof(D), 0);
        auto* header = new (raw) CallableHeader{&DestroyCallable<D>};
        new (static_cast<void*>(header + 1)) D(std::forward<F>(fn));
        lua_rawgetp(L_, LUA_REGISTRYINDEX, &kCallableMetaKey);
        lua_setmetatable(L_, -2);
        lua_pushcclosure(L_, &TrampCallable<D>, 1);
        const std::string key(name);
        lua_setglobal(L_, key.c_str());
    }

    template <class R, class... Args>
    Result<R> Call(std::string_view name, Args&&... args) const {
        return CallGlobal<R>(L_, name, std::forward<Args>(args)...);
    }

    // 逃生门：不捕获 Lua 错误，语义同 Function::CallUnprotected。
    template <class R, class... Args>
    R CallUnprotected(std::string_view name, Args&&... args) const {
        return CallGlobalUnprotected<R>(L_, name, std::forward<Args>(args)...);
    }

    // 定义在 class.h（建类器需要 ClassInfo 完整类型）。
    template <class T>
    ClassBuilder<T> Class(std::string_view name);

    void Enum(std::string_view name, std::initializer_list<std::pair<std::string_view, lua_Integer>> values) {
        const std::string key(name);
        lua_newtable(L_);
        for (const auto& [field, value] : values) {
            lua_pushlstring(L_, field.data(), field.size());
            lua_pushinteger(L_, value);
            lua_settable(L_, -3);
        }
        lua_setglobal(L_, key.c_str());
    }

    Result<void> RunScript(std::string_view source, const char* chunkname = "=fastlua");
    Result<void> RunFile(const char* path);
    Result<void> RunFile(const std::string& path) { return RunFile(path.c_str()); }

    void SetPath(std::string_view path);
    void AddPath(std::string_view path);

    void EnsureCallableMeta();

protected:
    lua_State* L_ = nullptr;
};

class LuaState : public LuaStateView {
public:
    explicit LuaState(const LuaStateOptions& options = {}) {
        if (options.allocator == nullptr) {
            L_ = luaL_newstate();
            FASTLUA_CHECK(L_ != nullptr, "fastlua: luaL_newstate failed");
            luaL_openlibs(L_);
        } else {
            L_ = lua_newstate(options.allocator, options.allocator_ud, luaL_makeseed(nullptr));
            FASTLUA_CHECK(L_ != nullptr, "fastlua: lua_newstate failed");
            luaL_openselectedlibs(L_, static_cast<int>(options.libs), 0);
        }
        lua_atpanic(L_, &PanicHandler);
        EnsureCallableMeta();
    }

    ~LuaState() {
        if (L_ != nullptr) {
            lua_close(L_);
            L_ = nullptr;
        }
    }

    LuaState(const LuaState&) = delete;
    LuaState& operator=(const LuaState&) = delete;
    LuaState(LuaState&& other) noexcept : LuaStateView(other.L_) { other.L_ = nullptr; }
    LuaState& operator=(LuaState&& other) noexcept {
        if (this != &other) {
            if (L_ != nullptr) {
                lua_close(L_);
            }
            L_ = other.L_;
            other.L_ = nullptr;
        }
        return *this;
    }

    static LuaStateView Attach(lua_State* L) noexcept { return LuaStateView(L); }
};

}  // namespace breeze::fastlua
