#pragma once

// 错误模型：
//   - 脚本/运行期错误 → Error + Result<T>（成功时零堆分配），或变成 Lua 错误。
//   - 用法错误 → BREEZE_LUA_CHECK（见 check.h）。
//   - 致命错误 → PanicHandler / Fatal。
// ErrorSlot 是 trampoline 在“不能 longjmp”的那一阶段用的平凡存储，
// 绝不能带析构，也不能分配。

#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "base/lua_binding/check.h"
#include "base/lua_binding/config.h"

namespace breeze::lua_binding {

enum class ErrorCode : std::uint8_t {
    kOk = 0,
    kNotFound,
    kRuntime,
    kSyntax,
    kMemory,
    kType,
    kStack,
    kCancelled,
};

struct Error {
    ErrorCode code = ErrorCode::kOk;
    std::string message;

    bool Ok() const noexcept { return code == ErrorCode::kOk; }
};

template <class T>
class Result {
public:
    Result(T value) : value_(std::move(value)) {}
    Result(Error error) : error_(std::move(error)) {}
    Result(ErrorCode code, std::string message) : error_{code, std::move(message)} {}

    bool Ok() const noexcept { return value_.has_value(); }
    explicit operator bool() const noexcept { return Ok(); }
    bool NotFound() const noexcept { return error_.code == ErrorCode::kNotFound; }

    const Error& GetError() const noexcept { return error_; }

    T& Value() & {
        BREEZE_LUA_CHECK(Ok(), "Result::Value() on error: {}", error_.message);
        return *value_;
    }
    const T& Value() const& {
        BREEZE_LUA_CHECK(Ok(), "Result::Value() on error: {}", error_.message);
        return *value_;
    }
    T&& Value() && {
        BREEZE_LUA_CHECK(Ok(), "Result::Value() on error: {}", error_.message);
        return std::move(*value_);
    }
    T ValueOr(T fallback) const {
        return Ok() ? *value_ : std::move(fallback);
    }

private:
    std::optional<T> value_;
    Error error_;
};

template <>
class Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)) {}
    Result(ErrorCode code, std::string message) : error_{code, std::move(message)} {}

    bool Ok() const noexcept { return error_.code == ErrorCode::kOk; }
    explicit operator bool() const noexcept { return Ok(); }
    bool NotFound() const noexcept { return error_.code == ErrorCode::kNotFound; }
    const Error& GetError() const noexcept { return error_; }

private:
    Error error_;
};

inline Result<void> Ok() { return Result<void>{}; }

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
    void Set(ErrorCode c, std::string_view msg) noexcept {
        code = c;
        arg = 0;
        expected = nullptr;
        const std::size_t n = msg.size() < sizeof(message) - 1 ? msg.size() : sizeof(message) - 1;
        for (std::size_t i = 0; i < n; ++i) {
            message[i] = msg[i];
        }
        message[n] = '\0';
    }
    void SetArg(int index, const char* expected_type) noexcept {
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
