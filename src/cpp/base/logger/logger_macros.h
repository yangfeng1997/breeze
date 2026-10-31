// 日志宏，两层：
//
//   内层 BREEZE_LOG_<LEVEL>(logger, fmt, ...)   第一个参数是 Logger 对象 / 指针 / 智能指针。
//   外层 LOG_<LEVEL>(fmt, ...)                  = 内层 + 默认 logger（g_default_logger，空则 stderr）。
//
//   LOG_INFO("player {} login from {}", id, ip);
//   BREEZE_LOG_INFO(resLogger, "load {} cost {}ms", name, ms);
//
// 业务层要给自己的 logger 配一族宏，直接套内层即可（编译期剥离随之生效）：
//   #define RES_LOG_INFO(...) BREEZE_LOG_INFO(::my::ResLogger(), __VA_ARGS__)
//
// 级别不够时参数不会被求值，也不会格式化。格式串在编译期检查。
//
// 编译期剥离：定义 BREEZE_LOG_ACTIVE_LEVEL 为 BREEZE_LOG_LEVEL_* 之一，低于它的宏展开成空
// （连参数一起消失，运行时再也开不出来）。默认全保留，由运行时 SetLevel 控制。
// FATAL 永远保留。
//
// 注意：<syslog.h> 也定义 LOG_INFO 等，同一个翻译单元里别同时包含。

#pragma once

#include <format>
#include <source_location>

#include "base/logger/logger.h"

// 编译期级别常量。注意与下面的 BREEZE_LOG_<LEVEL>(...) 宏区分：带 LEVEL_ 的是数字。
#define BREEZE_LOG_LEVEL_TRACE 0
#define BREEZE_LOG_LEVEL_DEBUG 1
#define BREEZE_LOG_LEVEL_INFO 2
#define BREEZE_LOG_LEVEL_WARN 3
#define BREEZE_LOG_LEVEL_ERROR 4
#define BREEZE_LOG_LEVEL_OFF 5

#ifndef BREEZE_LOG_ACTIVE_LEVEL
#define BREEZE_LOG_ACTIVE_LEVEL BREEZE_LOG_LEVEL_TRACE
#endif

// 内部实现：lg 求值得到 Logger / Logger* / 智能指针，只求值一次。
#define BREEZE_LOG_IMPL(lg, lvl, ...)                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        ::breeze::Logger& breeze_lg_ = ::breeze::detail::Ref(lg);                                                      \
        if (breeze_lg_.ShouldLog(lvl))                                                                                 \
            breeze_lg_.Log(lvl, ::std::source_location::current(), ::std::format(__VA_ARGS__));                        \
    } while (0)

// ---- 内层：指定 logger ----
#if BREEZE_LOG_ACTIVE_LEVEL <= BREEZE_LOG_LEVEL_TRACE
#define BREEZE_LOG_TRACE(lg, ...) BREEZE_LOG_IMPL(lg, ::breeze::Level::Trace, __VA_ARGS__)
#else
#define BREEZE_LOG_TRACE(lg, ...) ((void)0)
#endif

#if BREEZE_LOG_ACTIVE_LEVEL <= BREEZE_LOG_LEVEL_DEBUG
#define BREEZE_LOG_DEBUG(lg, ...) BREEZE_LOG_IMPL(lg, ::breeze::Level::Debug, __VA_ARGS__)
#else
#define BREEZE_LOG_DEBUG(lg, ...) ((void)0)
#endif

#if BREEZE_LOG_ACTIVE_LEVEL <= BREEZE_LOG_LEVEL_INFO
#define BREEZE_LOG_INFO(lg, ...) BREEZE_LOG_IMPL(lg, ::breeze::Level::Info, __VA_ARGS__)
#else
#define BREEZE_LOG_INFO(lg, ...) ((void)0)
#endif

#if BREEZE_LOG_ACTIVE_LEVEL <= BREEZE_LOG_LEVEL_WARN
#define BREEZE_LOG_WARN(lg, ...) BREEZE_LOG_IMPL(lg, ::breeze::Level::Warn, __VA_ARGS__)
#else
#define BREEZE_LOG_WARN(lg, ...) ((void)0)
#endif

#if BREEZE_LOG_ACTIVE_LEVEL <= BREEZE_LOG_LEVEL_ERROR
#define BREEZE_LOG_ERROR(lg, ...) BREEZE_LOG_IMPL(lg, ::breeze::Level::Error, __VA_ARGS__)
#else
#define BREEZE_LOG_ERROR(lg, ...) ((void)0)
#endif

#define BREEZE_LOG_FATAL(lg, ...)                                                                                      \
    ::breeze::detail::Ref(lg).Fatal(::std::source_location::current(), ::std::format(__VA_ARGS__))

// ---- 外层：默认 logger ----
#define LOG_TRACE(...) BREEZE_LOG_TRACE(::breeze::CurrentLogger(), __VA_ARGS__)
#define LOG_DEBUG(...) BREEZE_LOG_DEBUG(::breeze::CurrentLogger(), __VA_ARGS__)
#define LOG_INFO(...) BREEZE_LOG_INFO(::breeze::CurrentLogger(), __VA_ARGS__)
#define LOG_WARN(...) BREEZE_LOG_WARN(::breeze::CurrentLogger(), __VA_ARGS__)
#define LOG_ERROR(...) BREEZE_LOG_ERROR(::breeze::CurrentLogger(), __VA_ARGS__)
#define LOG_FATAL(...) BREEZE_LOG_FATAL(::breeze::CurrentLogger(), __VA_ARGS__)
