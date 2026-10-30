// 当前线程调用栈的文本化，供断言失败、致命错误时留现场。只支持 Linux / glibc（libgcc 的 _Unwind_Backtrace + dladdr）。
// 不用 glibc 的 backtrace()：ASan 会拦截它，栈顶多出一层拦截器帧，skip 的帧数就不准了；_Unwind_Backtrace 没这个问题。

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

// 把一个返回地址格式化成上面的一行（不带换行）。返回地址、不是调用指令的地址：
// 调用点是上一个函数的最后一条指令时（常见于 noreturn 调用），返回地址已经落进下一个函数，所以查符号用 pc-1。
// 自己抓栈（比如崩溃处理里先存地址、事后再格式化）时用。
std::string FormatBacktraceFrame(std::size_t index, const void* return_address);

} // namespace breeze
