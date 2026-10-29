// 当前线程调用栈的文本化，供断言失败、致命错误时留现场。只支持 Linux / glibc（backtrace + dladdr）。

#pragma once

#include <cstddef>
#include <string>

namespace breeze
{

// 每帧一行，帧间 '\n'，末尾不带换行：
//   #0 0x000055d4c4f5a123 breeze::Foo::Bar(int)+0x1a (/path/prog+0x2123)
// 括号里是 模块+模块内偏移，符号被剥离或没导出时可离线还原：addr2line -f -C -e <模块> <偏移>。
//
// 符号名靠 dladdr，只认动态符号表：可执行文件要带 -rdynamic（CMake: ENABLE_EXPORTS ON），
// 否则可执行文件自己的函数只有地址与偏移；共享库的导出符号不受影响。
//
// skip 跳过栈顶 skip 帧（不含 Backtrace 自己）；最多 max_depth 帧，上限 kMaxFrames。
// 不是 async-signal-safe（malloc、dladdr 内部加锁）：别在信号处理函数里调。
inline constexpr std::size_t kMaxBacktraceFrames = 128;
[[gnu::noinline]] std::string Backtrace(std::size_t max_depth = 32, std::size_t skip = 0);

} // namespace breeze
