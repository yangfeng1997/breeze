#pragma once

// fastlua 自带的断言与日志，不依赖 breeze 的 base/assert。
// 三类错误（对应 docs/Lua绑定库设计.md）：
//   FASTLUA_CHECK    用法错误，任何构建都致命：先调注入的 fatal handler，再 abort。
//   FASTLUA_DCHECK   内部不变量，仅 Debug 生效。
//   FASTLUA_ENSURE   不该发生但可以继续：每处调用点只记一次日志，然后继续。
// 日志与致命处理通过 SetLogSink / SetFatalHandler 注入；默认写 stderr。
// 这样 base 层不依赖 framework 的 logger，framework 以后可以把它们接过去。

#include <atomic>
#include <format>
#include <string>
#include <string_view>

#include "base/fastlua/config.h"

namespace breeze::fastlua {

using LogSink = void (*)(std::string_view message) noexcept;
using FatalHandler = void (*)(std::string_view message) noexcept;

void SetLogSink(LogSink sink) noexcept;
void SetFatalHandler(FatalHandler handler) noexcept;
void LogMessage(std::string_view message) noexcept;
[[noreturn]] void Fatal(std::string_view message) noexcept;

// 冷路径专用：只在错误/日志里格式化，热路径不要调用。
template <class... Args>
std::string Format(std::format_string<Args...> fmt, Args&&... args) {
    return std::format(fmt, std::forward<Args>(args)...);
}

}  // namespace breeze::fastlua

#define FASTLUA_CHECK(cond, ...)                                                        \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            ::breeze::fastlua::Fatal(::breeze::fastlua::Format(__VA_ARGS__));           \
        }                                                                               \
    } while (0)

#define FASTLUA_DCHECK(cond, ...)                                                       \
    do {                                                                                \
        if constexpr (::breeze::fastlua::kDebugBuild) {                                 \
            if (!(cond)) {                                                              \
                ::breeze::fastlua::Fatal(::breeze::fastlua::Format(__VA_ARGS__));       \
            }                                                                           \
        }                                                                               \
    } while (0)

#define FASTLUA_ENSURE(cond, ...)                                                       \
    do {                                                                                \
        static ::std::atomic<bool> fastlua_ensure_reported_{false};                     \
        if (!(cond) && !fastlua_ensure_reported_.exchange(true, ::std::memory_order_relaxed)) { \
            ::breeze::fastlua::LogMessage(::breeze::fastlua::Format(__VA_ARGS__));      \
        }                                                                               \
    } while (0)
