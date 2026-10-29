// 防御式断言：开发期失败即崩，线上失败不宕机、走防御式控制流。
//
//   ASSERT(expr)                    失败：Debug abort | Release 只记日志，继续往下走
//   ASSERT_RET_VOID(expr)           失败：Debug abort | Release 记日志后 return
//   ASSERT_RET_VAL(expr, val)       失败：Debug abort | Release 记日志后 return val
//   ASSERT_CONTINUE(expr)           失败：Debug abort | Release 记日志后 continue
//   ASSERT_BREAK(expr)              失败：Debug abort | Release 记日志后 break
//
//   ASSERT_RET_VOID(player);
//   ASSERT_RET_VAL(player->IsOnline(), false);
//   for (auto& item : items) { ASSERT_CONTINUE(item != nullptr); item->Update(); }
//
// - expr 两种模式下都会求值（有副作用也照常执行），别指望 Release 里它被去掉。
// - 与 LOG_FATAL 的分工：LOG_FATAL 是"无论如何都不能继续"，ASSERT 是"这是 bug，但一个玩家的 bug 不该拉倒整个进程"。
// - 宏是独立语句，不是表达式：不能写在没有花括号的 if / else 分支里（会编译失败，不会悄悄绑错 else）。
//
// Debug（未定义 NDEBUG）：经默认 logger 写 Fatal（表达式、位置、调用栈），排空日志队列后 abort。
// Release：每个调用点各自计数，只在第 1、2、4、8…次失败时记一条 Error（带次数与调用栈），
//          热路径上的 bug 不会把日志刷爆。日志走默认 logger，没有则 stderr（见 logger.h 的 CurrentLogger）。
// 编译时定义 BREEZE_ASSERT_FATAL=0/1 可以不看 NDEBUG 强制选择模式（预发布环境想让 Release 也崩，或测试两条路径）。
//
// 文件名带 _macros 是因为不能叫 assert.h：本目录在 include 路径里，会盖掉系统的 <assert.h>。

#pragma once

#include <atomic>
#include <cstdint>
#include <source_location>

#ifndef BREEZE_ASSERT_FATAL
#ifdef NDEBUG
#define BREEZE_ASSERT_FATAL 0
#else
#define BREEZE_ASSERT_FATAL 1
#endif
#endif

namespace breeze::detail
{

// 写 Fatal 日志并 abort，不返回。
[[noreturn]] void AssertAbort(std::source_location loc, const char* expr);

// hits 是该调用点自己的失败计数；只在第 2^k 次失败时真正写日志。
void AssertReport(std::atomic<std::uint32_t>& hits, std::source_location loc, const char* expr);

} // namespace breeze::detail

#if BREEZE_ASSERT_FATAL
#define BREEZE_ASSERT_FAILED_(expr_str) ::breeze::detail::AssertAbort(::std::source_location::current(), expr_str)
#else
// 计数器是调用点私有的函数内 static：常量初始化，没有"首次进入"的守卫开销。
#define BREEZE_ASSERT_FAILED_(expr_str)                                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        static ::std::atomic<::std::uint32_t> breeze_assert_hits_{0};                                                  \
        ::breeze::detail::AssertReport(breeze_assert_hits_, ::std::source_location::current(), expr_str);              \
    } while (0)
#endif

// 普通语句位置用：包 do/while，失败后执行 action。
#define BREEZE_ASSERT_DO_(expr, action)                                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expr)) [[unlikely]]                                                                                      \
        {                                                                                                              \
            BREEZE_ASSERT_FAILED_(#expr);                                                                              \
            action;                                                                                                    \
        }                                                                                                              \
    } while (0)

// 循环里用：不能包 do/while，否则 continue / break 作用到那层 do/while 上，不是外面的循环。
#define BREEZE_ASSERT_LOOP_(expr, action)                                                                              \
    if (!(expr)) [[unlikely]]                                                                                          \
    {                                                                                                                  \
        BREEZE_ASSERT_FAILED_(#expr);                                                                                  \
        action;                                                                                                        \
    }

#define ASSERT(expr) BREEZE_ASSERT_DO_(expr, (void)0)
#define ASSERT_RET_VOID(expr) BREEZE_ASSERT_DO_(expr, return )
#define ASSERT_RET_VAL(expr, val) BREEZE_ASSERT_DO_(expr, return (val))
#define ASSERT_CONTINUE(expr) BREEZE_ASSERT_LOOP_(expr, continue)
#define ASSERT_BREAK(expr) BREEZE_ASSERT_LOOP_(expr, break)
