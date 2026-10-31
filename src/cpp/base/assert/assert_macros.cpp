#include "base/assert/assert_macros.h"

#include <format>
#include <string>

#include "base/backtrace/backtrace.h"
#include "base/logger/logger_macros.h"

namespace breeze::detail
{

namespace
{

constexpr std::size_t kStackDepth = 24;

// 帧序：Stack、AssertAbort/AssertReport、宏展开处的调用方…… 跳过前两个，栈顶就是调用方。
// noinline 保证 Stack 自己占一帧，skip 的数才算得准。
[[gnu::noinline]] std::string Stack()
{
    std::string s = Backtrace(kStackDepth, 2);
    asm volatile("" : "+m"(s)); // 调用不能落在尾位置，否则优化器会把本帧吃掉，skip 就差一帧
    return s;
}

} // namespace

void AssertAbort(std::source_location loc, const char* expr)
{
    // Fatal 先排空日志队列再直写 abort；多行文本放进同一条消息，不会被别的线程的日志切开。
    CurrentLogger().Fatal(
        loc, std::format("ASSERT failed: ({})\n  at {}\n  backtrace:\n{}", expr, loc.function_name(), Stack()));
}

void AssertReport(std::atomic<std::uint32_t>& hits, std::source_location loc, const char* expr)
{
    const std::uint32_t n = hits.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((n & (n - 1)) != 0)
        return; // 只在 1、2、4、8… 次记

    CurrentLogger().Log(
        Level::Error,
        loc.file_name(),
        static_cast<int>(loc.line()),
        loc.function_name(),
        std::format(
            "ASSERT failed: ({}) 本调用点第 {} 次失败（只在 2 的幂次记录）\n  backtrace:\n{}", expr, n, Stack()));
}

} // namespace breeze::detail
