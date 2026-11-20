#pragma once

// 错误模型（2026-11-20：不再自造 Result，统一用 std::expected）：
//   - 脚本/运行期错误 → std::expected<T, Error>，或变成 Lua 错误。
//   - 用法错误 → BREEZE_LUA_CHECK（见 check.h）。
//   - 致命错误 → PanicHandler / Fatal。
// ErrorSlot 是 trampoline 在“不能 longjmp”的那一阶段用的平凡存储，
// 绝不能带析构，也不能分配。
//
// 为什么删掉原来的 Result：std::expected 在成功路径上不构造 Error（union），同样零堆分配，
// 而且能和框架其它模块共用同一套机制；原来的辅助方法收敛成下面的自由函数。

#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include "base/lua_binding/check.h"
#include "base/lua_binding/config.h"

namespace breeze::lua_binding {

enum class ErrorCode : std::uint8_t {
    kOk = 0,
    kNotFound,
    kLuaError,      // Lua VM 报错（pcall/load/coroutine resume）
    kInvalidState,  // 对象状态误用：state 已关闭、协程不可恢复
    kSyntax,
    kMemory,
    kType,
    kStack,
    kCancelled,
};

struct Error {
    ErrorCode code = ErrorCode::kOk;
    std::string message;

    bool IsOk() const noexcept { return code == ErrorCode::kOk; }
};

// 统一的失败构造：原 Result<T>(code, msg) 的替代。
inline std::unexpected<Error> Fail(ErrorCode code, std::string message) {
    return std::unexpected<Error>(Error{code, std::move(message)});
}

// 原 Result::NotFound() 的替代。两种入参都收：Error 本身，或 expected（失败且码是 kNotFound）。
inline bool IsNotFound(const Error& error) noexcept { return error.code == ErrorCode::kNotFound; }

template <class T>
bool IsNotFound(const std::expected<T, Error>& result) noexcept {
    return !result.has_value() && result.error().code == ErrorCode::kNotFound;
}

// 原 Result::Value() 的替代：失败时带消息 abort（诊断和以前一致）。
// 正确用法仍然是先判 has_value()；不要把 *r 当 Value() 用（错误时是 UB）。
template <class T>
T& ValueOrDie(std::expected<T, Error>& result) {
    BREEZE_LUA_CHECK(result.has_value(), "ValueOrDie() on error: {}", result.error().message);
    return *result;
}

template <class T>
const T& ValueOrDie(const std::expected<T, Error>& result) {
    BREEZE_LUA_CHECK(result.has_value(), "ValueOrDie() on error: {}", result.error().message);
    return *result;
}

template <class T>
T ValueOrDie(std::expected<T, Error>&& result) {
    BREEZE_LUA_CHECK(result.has_value(), "ValueOrDie() on error: {}", result.error().message);
    return std::move(*result);
}

// trampoline 的平凡存储。只接受平凡可析构的类型。
struct ErrorSlot {
    ErrorCode code = ErrorCode::kOk;
    int arg = 0;                  // 1-based 参数下标；0 表示不是参数错误
    const char* expected = nullptr;
    char message[192] = {};

    bool Failed() const noexcept { return code != ErrorCode::kOk; }
    void Clear() noexcept {
        code = ErrorCode::kOk;
        arg = 0;
        expected = nullptr;
        message[0] = '\0';
    }
    void SetError(ErrorCode c, std::string_view msg) noexcept {
        code = c;
        arg = 0;
        expected = nullptr;
        const std::size_t n = msg.size() < sizeof(message) - 1 ? msg.size() : sizeof(message) - 1;
        for (std::size_t i = 0; i < n; ++i) {
            message[i] = msg[i];
        }
        message[n] = '\0';
    }
    void SetTypeError(int index, const char* expected_type) noexcept {
        code = ErrorCode::kType;
        arg = index;
        expected = expected_type;
        message[0] = '\0';
    }
};

// 在只含平凡局部变量的那一层调用；内部会 lua_error（longjmp）。
[[noreturn]] void RaiseError(lua_State* L, const ErrorSlot& slot);
int PanicHandler(lua_State* L);

}  // namespace breeze::lua_binding
