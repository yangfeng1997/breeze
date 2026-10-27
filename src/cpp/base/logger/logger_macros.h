// 日志宏。
//
//   LOG_INFO("player {} login from {}", id, ip);          // 默认 logger（g_logger，空则 stderr）
//   LOG_INFO_TO(logger, "player {} login", id);           // 指定 logger：对象 / 指针 / 智能指针
//
// 级别不够时参数不会被求值，也不会格式化。格式串在编译期检查。
//
// 编译期剥离：定义 BREEZE_LOG_ACTIVE_LEVEL 为 BREEZE_LOG_LEVEL_* 之一，低于它的宏展开成空
// （连参数一起消失，运行时再也开不出来）。默认全保留，由运行时 SetLevel 控制。
// LOG_FATAL 永远保留。
//
// 注意：<syslog.h> 也定义 LOG_INFO 等，同一个翻译单元里别同时包含。

#pragma once

#include <format>
#include <source_location>

#include "logger.h"

#define BREEZE_LOG_LEVEL_TRACE 0
#define BREEZE_LOG_LEVEL_DEBUG 1
#define BREEZE_LOG_LEVEL_INFO 2
#define BREEZE_LOG_LEVEL_WARN 3
#define BREEZE_LOG_LEVEL_ERROR 4
#define BREEZE_LOG_LEVEL_OFF 5

#ifndef BREEZE_LOG_ACTIVE_LEVEL
#define BREEZE_LOG_ACTIVE_LEVEL BREEZE_LOG_LEVEL_TRACE
#endif

// lgexpr 是求值得到 Logger& 的表达式。
#define BREEZE_LOG_IMPL(lgexpr, lvl, ...)                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        ::breeze::Logger& breeze_lg_ = (lgexpr);                                                                       \
        if (breeze_lg_.ShouldLog(lvl))                                                                                 \
            breeze_lg_.Log(lvl, ::std::source_location::current(), ::std::format(__VA_ARGS__));                        \
    } while (0)

#define BREEZE_LOG_DEFAULT(lvl, ...) BREEZE_LOG_IMPL(::breeze::CurrentLogger(), lvl, __VA_ARGS__)
#define BREEZE_LOG_TO(lg, lvl, ...) BREEZE_LOG_IMPL(::breeze::detail::Ref(lg), lvl, __VA_ARGS__)

#if BREEZE_LOG_ACTIVE_LEVEL <= BREEZE_LOG_LEVEL_TRACE
#define LOG_TRACE(...) BREEZE_LOG_DEFAULT(::breeze::Level::Trace, __VA_ARGS__)
#define LOG_TRACE_TO(lg, ...) BREEZE_LOG_TO(lg, ::breeze::Level::Trace, __VA_ARGS__)
#else
#define LOG_TRACE(...) ((void)0)
#define LOG_TRACE_TO(lg, ...) ((void)0)
#endif

#if BREEZE_LOG_ACTIVE_LEVEL <= BREEZE_LOG_LEVEL_DEBUG
#define LOG_DEBUG(...) BREEZE_LOG_DEFAULT(::breeze::Level::Debug, __VA_ARGS__)
#define LOG_DEBUG_TO(lg, ...) BREEZE_LOG_TO(lg, ::breeze::Level::Debug, __VA_ARGS__)
#else
#define LOG_DEBUG(...) ((void)0)
#define LOG_DEBUG_TO(lg, ...) ((void)0)
#endif

#if BREEZE_LOG_ACTIVE_LEVEL <= BREEZE_LOG_LEVEL_INFO
#define LOG_INFO(...) BREEZE_LOG_DEFAULT(::breeze::Level::Info, __VA_ARGS__)
#define LOG_INFO_TO(lg, ...) BREEZE_LOG_TO(lg, ::breeze::Level::Info, __VA_ARGS__)
#else
#define LOG_INFO(...) ((void)0)
#define LOG_INFO_TO(lg, ...) ((void)0)
#endif

#if BREEZE_LOG_ACTIVE_LEVEL <= BREEZE_LOG_LEVEL_WARN
#define LOG_WARN(...) BREEZE_LOG_DEFAULT(::breeze::Level::Warn, __VA_ARGS__)
#define LOG_WARN_TO(lg, ...) BREEZE_LOG_TO(lg, ::breeze::Level::Warn, __VA_ARGS__)
#else
#define LOG_WARN(...) ((void)0)
#define LOG_WARN_TO(lg, ...) ((void)0)
#endif

#if BREEZE_LOG_ACTIVE_LEVEL <= BREEZE_LOG_LEVEL_ERROR
#define LOG_ERROR(...) BREEZE_LOG_DEFAULT(::breeze::Level::Error, __VA_ARGS__)
#define LOG_ERROR_TO(lg, ...) BREEZE_LOG_TO(lg, ::breeze::Level::Error, __VA_ARGS__)
#else
#define LOG_ERROR(...) ((void)0)
#define LOG_ERROR_TO(lg, ...) ((void)0)
#endif

#define LOG_FATAL(...) ::breeze::CurrentLogger().Fatal(::std::source_location::current(), ::std::format(__VA_ARGS__))
#define LOG_FATAL_TO(lg, ...)                                                                                          \
    ::breeze::detail::Ref(lg).Fatal(::std::source_location::current(), ::std::format(__VA_ARGS__))
