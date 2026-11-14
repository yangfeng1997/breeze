#pragma once

// 栈转换。约定：
//   Stack<T>::Check(L, idx)  只读、不抛错、可分配 Lua 栈空间；失败返回 false。
//   Stack<T>::Get(L, idx)    只在 Check 成功后调用；失败是调用方的 bug。
//   Stack<T>::Push(L, v)     推入一个值（tuple/pair 除外：它们推入多个，只用于返回值）。
//   Stack<T>::kName          错误信息里的类型名。
//
// 参数统一走“先全部 Check、再全部 Get”（见 trampoline.h），所以 Get 不做检查，
// 也不会在检查阶段留下需要析构的临时对象。

#include <limits>
#include <type_traits>
#include <utility>

#include "base/fastlua/config.h"
#include "base/fastlua/error.h"

namespace breeze::fastlua {

template <class T, class Enable = void>
struct Stack;

template <class T>
struct is_tuple_like : std::false_type {};
template <class... A>
struct is_tuple_like<std::tuple<A...>> : std::true_type {};
template <class A, class B>
struct is_tuple_like<std::pair<A, B>> : std::true_type {};

template <class T>
inline constexpr bool is_tuple_like_v = is_tuple_like<std::remove_cvref_t<T>>::value;

template <class T, class Enable>
struct Stack {
    static constexpr const char* kName = "value";
};

// ---------- 整数 ----------
template <class T>
struct Stack<T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>> {
    static constexpr const char* kName = std::is_signed_v<T> ? "integer" : "unsigned integer";

    static bool Check(lua_State* L, int idx) {
        int ok = 0;
        lua_Integer value = lua_tointegerx(L, idx, &ok);
        if (ok == 0) {
            return false;
        }
        if constexpr (std::is_signed_v<T>) {
            return value >= static_cast<lua_Integer>(std::numeric_limits<T>::min()) &&
                   value <= static_cast<lua_Integer>(std::numeric_limits<T>::max());
        } else {
            return value >= 0 &&
                   static_cast<std::uint64_t>(value) <= static_cast<std::uint64_t>(std::numeric_limits<T>::max());
        }
    }

    static T Get(lua_State* L, int idx) {
        int ok = 0;
        lua_Integer value = lua_tointegerx(L, idx, &ok);
        (void)ok;
        return static_cast<T>(value);
    }

    static void Push(lua_State* L, T value) {
        lua_pushinteger(L, static_cast<lua_Integer>(value));
    }
};

// ---------- 浮点 ----------
template <class T>
struct Stack<T, std::enable_if_t<std::is_floating_point_v<T>>> {
    static constexpr const char* kName = "number";

    static bool Check(lua_State* L, int idx) { return lua_type(L, idx) == LUA_TNUMBER; }

    static T Get(lua_State* L, int idx) {
        int ok = 0;
        lua_Number value = lua_tonumberx(L, idx, &ok);
        (void)ok;
        return static_cast<T>(value);
    }

    static void Push(lua_State* L, T value) { lua_pushnumber(L, static_cast<lua_Number>(value)); }
};

// ---------- bool ----------
template <>
struct Stack<bool, void> {
    static constexpr const char* kName = "boolean";

    static bool Check(lua_State* L, int idx) { return lua_type(L, idx) == LUA_TBOOLEAN; }
    static bool Get(lua_State* L, int idx) { return lua_toboolean(L, idx) != 0; }
    static void Push(lua_State* L, bool value) { lua_pushboolean(L, value ? 1 : 0); }
};

// ---------- enum（按底层整数） ----------
template <class T>
struct Stack<T, std::enable_if_t<std::is_enum_v<T>>> {
    using U = std::underlying_type_t<T>;
    static constexpr const char* kName = Stack<U>::kName;

    static bool Check(lua_State* L, int idx) { return Stack<U>::Check(L, idx); }
    static T Get(lua_State* L, int idx) { return static_cast<T>(Stack<U>::Get(L, idx)); }
    static void Push(lua_State* L, T value) { Stack<U>::Push(L, static_cast<U>(value)); }
};

// ---------- 字符串 ----------
template <>
struct Stack<std::string, void> {
    static constexpr const char* kName = "string";

    static bool Check(lua_State* L, int idx) {
        const int type = lua_type(L, idx);
        return type == LUA_TSTRING || type == LUA_TNUMBER;
    }
    static std::string Get(lua_State* L, int idx) {
        std::size_t length = 0;
        const char* data = lua_tolstring(L, idx, &length);
        return data != nullptr ? std::string(data, length) : std::string();
    }
    static void Push(lua_State* L, std::string_view value) { lua_pushlstring(L, value.data(), value.size()); }
};

template <>
struct Stack<std::string_view, void> {
    static constexpr const char* kName = "string";

    // 视图只在“该值仍留在栈上”期间有效；作为参数时正好覆盖整个调用。
    static bool Check(lua_State* L, int idx) { return lua_type(L, idx) == LUA_TSTRING; }
    static std::string_view Get(lua_State* L, int idx) {
        std::size_t length = 0;
        const char* data = lua_tolstring(L, idx, &length);
        return data != nullptr ? std::string_view(data, length) : std::string_view();
    }
    static void Push(lua_State* L, std::string_view value) { lua_pushlstring(L, value.data(), value.size()); }
};

template <>
struct Stack<const char*, void> {
    static constexpr const char* kName = "string";

    static bool Check(lua_State* L, int idx) {
        const int type = lua_type(L, idx);
        return type == LUA_TSTRING || type == LUA_TNIL;
    }
    static const char* Get(lua_State* L, int idx) { return lua_tostring(L, idx); }
    static void Push(lua_State* L, const char* value) {
        if (value != nullptr) {
            lua_pushstring(L, value);
        } else {
            lua_pushnil(L);
        }
    }
};

template <>
struct Stack<char*, void> : Stack<const char*> {};

// ---------- nil ----------
template <>
struct Stack<std::nullptr_t, void> {
    static constexpr const char* kName = "nil";

    static bool Check(lua_State* L, int idx) { return lua_type(L, idx) == LUA_TNIL; }
    static std::nullptr_t Get(lua_State*, int) { return nullptr; }
    static void Push(lua_State* L, std::nullptr_t) { lua_pushnil(L); }
};

// ---------- optional ----------
template <class T>
struct Stack<std::optional<T>, void> {
    static constexpr const char* kName = Stack<T>::kName;

    static bool Check(lua_State* L, int idx) {
        return lua_type(L, idx) == LUA_TNIL || Stack<T>::Check(L, idx);
    }
    static std::optional<T> Get(lua_State* L, int idx) {
        if (lua_type(L, idx) == LUA_TNIL) {
            return std::nullopt;
        }
        return Stack<T>::Get(L, idx);
    }
    static void Push(lua_State* L, const std::optional<T>& value) {
        if (value.has_value()) {
            Stack<T>::Push(L, *value);
        } else {
            lua_pushnil(L);
        }
    }
};

// ---------- vector ----------
template <class T>
struct Stack<std::vector<T>, void> {
    static constexpr const char* kName = "table";

    static bool Check(lua_State* L, int idx) {
        idx = lua_absindex(L, idx);
        if (!lua_istable(L, idx)) {
            return false;
        }
        const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, idx));
        if (!lua_checkstack(L, 2)) {
            return false;
        }
        for (lua_Integer i = 1; i <= n; ++i) {
            lua_rawgeti(L, idx, i);
            const bool ok = Stack<T>::Check(L, -1);
            lua_pop(L, 1);
            if (!ok) {
                return false;
            }
        }
        return true;
    }

    static std::vector<T> Get(lua_State* L, int idx) {
        idx = lua_absindex(L, idx);
        const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, idx));
        std::vector<T> out;
        out.reserve(static_cast<std::size_t>(n));
        for (lua_Integer i = 1; i <= n; ++i) {
            lua_rawgeti(L, idx, i);
            out.push_back(Stack<T>::Get(L, -1));
            lua_pop(L, 1);
        }
        return out;
    }

    static void Push(lua_State* L, const std::vector<T>& values) {
        lua_createtable(L, static_cast<int>(values.size()), 0);
        for (std::size_t i = 0; i < values.size(); ++i) {
            Stack<T>::Push(L, values[i]);
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
    }
};

// ---------- map ----------
template <class K, class V>
struct Stack<std::map<K, V>, void> {
    static constexpr const char* kName = "table";

    static bool Check(lua_State* L, int idx) {
        idx = lua_absindex(L, idx);
        if (!lua_istable(L, idx) || !lua_checkstack(L, 2)) {
            return false;
        }
        lua_pushnil(L);
        while (lua_next(L, idx) != 0) {
            const bool ok = Stack<K>::Check(L, -2) && Stack<V>::Check(L, -1);
            lua_pop(L, 1);
            if (!ok) {
                lua_pop(L, 1);
                return false;
            }
        }
        return true;
    }

    static std::map<K, V> Get(lua_State* L, int idx) {
        idx = lua_absindex(L, idx);
        std::map<K, V> out;
        lua_pushnil(L);
        while (lua_next(L, idx) != 0) {
            K key = Stack<K>::Get(L, -2);
            V value = Stack<V>::Get(L, -1);
            out.emplace(std::move(key), std::move(value));
            lua_pop(L, 1);
        }
        return out;
    }

    static void Push(lua_State* L, const std::map<K, V>& values) {
        lua_createtable(L, 0, static_cast<int>(values.size()));
        for (const auto& [key, value] : values) {
            Stack<K>::Push(L, key);
            Stack<V>::Push(L, value);
            lua_settable(L, -3);
        }
    }
};

// tuple / pair：只用于返回值（多值返回）；作为参数会被 PushArgs 拒绝。
template <class... A>
struct Stack<std::tuple<A...>, void> {
    static constexpr const char* kName = "tuple";
    static constexpr int kPushCount = static_cast<int>(sizeof...(A));

    static bool Check(lua_State*, int) { return false; }
    static std::tuple<A...> Get(lua_State*, int) = delete;

    static void Push(lua_State* L, const std::tuple<A...>& values) {
        std::apply([&](const auto&... item) { (Stack<std::remove_cvref_t<decltype(item)>>::Push(L, item), ...); },
                   values);
    }
};

template <class A, class B>
struct Stack<std::pair<A, B>, void> {
    static constexpr const char* kName = "pair";
    static constexpr int kPushCount = 2;

    static bool Check(lua_State*, int) { return false; }
    static std::pair<A, B> Get(lua_State*, int) = delete;

    static void Push(lua_State* L, const std::pair<A, B>& value) {
        Stack<A>::Push(L, value.first);
        Stack<B>::Push(L, value.second);
    }
};

// ---------- 参数/返回值 ----------
template <class... Args>
void PushArgs(lua_State* L, Args&&... args) {
    static_assert((!is_tuple_like_v<Args> && ...),
                  "fastlua: tuple/pair can only be returned, not passed as an argument; use a table");
    (Stack<std::decay_t<Args>>::Push(L, std::forward<Args>(args)), ...);
}

template <class R>
int PushReturn(lua_State* L, R&& value) {
    using D = std::remove_cvref_t<R>;
    if constexpr (std::is_void_v<D>) {
        return 0;
    } else if constexpr (is_tuple_like_v<D>) {
        std::apply(
            [&](auto&&... item) {
                (Stack<std::remove_cvref_t<decltype(item)>>::Push(L, std::forward<decltype(item)>(item)), ...);
            },
            std::forward<R>(value));
        return static_cast<int>(std::tuple_size_v<D>);
    } else {
        Stack<D>::Push(L, std::forward<R>(value));
        return 1;
    }
}

}  // namespace breeze::fastlua
