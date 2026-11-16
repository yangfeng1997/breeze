#pragma once

// 自动元方法：注册类时按 C++ 能力挂 __tostring / __eq / __lt / __close。
// 检测规则与限制见 docs/Lua绑定库设计.md；未覆盖的元方法用 ClassBuilder::MetaMethod 显式注册。

#include <concepts>
#include <exception>
#include <ostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

#include "base/lua_binding/check.h"
#include "base/lua_binding/config.h"
#include "base/lua_binding/error.h"
#include "base/lua_binding/object.h"

namespace breeze::lua_binding {

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
            err.Set(ErrorCode::kType, "breeze::lua_binding: attempt to compare incompatible objects");
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
            err.Set(ErrorCode::kType, "breeze::lua_binding: __close on an invalid object");
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

}  // namespace breeze::lua_binding
