#include "backtrace.h"

#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace breeze
{

namespace
{

void AppendFrame(std::string& out, std::size_t index, void* pc)
{
    char head[48];
    std::snprintf(head, sizeof(head), "#%zu 0x%016lx", index, reinterpret_cast<std::uintptr_t>(pc));
    out += head;

    // 返回地址可能已经落在下一个函数里（调用点是上一个函数的最后一条指令，常见于 noreturn 调用），pc-1 才在调用方内部。
    Dl_info info{};
    const bool found = ::dladdr(static_cast<char*>(pc) - 1, &info) != 0;

    if (found && info.dli_sname != nullptr)
    {
        int status = 0;
        // __cxa_demangle 的结果是 malloc 出来的，必须 free
        std::unique_ptr<char, decltype(&std::free)> demangled(abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status),
                                                              &std::free);
        out += ' ';
        out += (status == 0 && demangled) ? demangled.get() : info.dli_sname;
        std::snprintf(head, sizeof(head), "+0x%lx",
                      static_cast<unsigned long>(static_cast<char*>(pc) - static_cast<char*>(info.dli_saddr)));
        out += head;
    }

    if (found && info.dli_fname != nullptr)
    {
        std::snprintf(head, sizeof(head), "+0x%lx)",
                      static_cast<unsigned long>(static_cast<char*>(pc) - static_cast<char*>(info.dli_fbase)));
        out += " (";
        out += info.dli_fname;
        out += head;
    }
}

} // namespace

std::string Backtrace(std::size_t max_depth, std::size_t skip)
{
    if (max_depth == 0 || skip >= kMaxBacktraceFrames)
        return {};

    // +1 跳过 Backtrace 自己这一帧
    const std::size_t skipped = skip + 1;
    void* frames[kMaxBacktraceFrames];
    const std::size_t want = std::min(skipped + max_depth, kMaxBacktraceFrames);
    const int got = ::backtrace(frames, static_cast<int>(want));
    if (got <= 0 || static_cast<std::size_t>(got) <= skipped)
        return {};

    const auto end = static_cast<std::size_t>(got);
    std::string out;
    out.reserve((end - skipped) * 96);
    for (std::size_t i = skipped; i < end; ++i)
    {
        if (i != skipped)
            out += '\n';
        AppendFrame(out, i - skipped, frames[i]);
    }
    return out;
}

} // namespace breeze
