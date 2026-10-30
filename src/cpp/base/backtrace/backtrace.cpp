#include "backtrace.h"

#include <cxxabi.h>
#include <dlfcn.h>
#include <unwind.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace breeze
{

namespace
{

void AppendFrame(std::string& out, std::size_t index, const void* pc_in)
{
    const char* pc = static_cast<const char*>(pc_in);
    char head[48];
    std::snprintf(head, sizeof(head), "#%zu 0x%016lx", index, reinterpret_cast<std::uintptr_t>(pc));
    out += head;

    // 返回地址可能已经落在下一个函数里（调用点是上一个函数的最后一条指令，常见于 noreturn 调用），pc-1 才在调用方内部。
    Dl_info info{};
    const bool found = ::dladdr(pc - 1, &info) != 0;

    if (found && info.dli_sname != nullptr)
    {
        int status = 0;
        // __cxa_demangle 的结果是 malloc 出来的，必须 free
        std::unique_ptr<char, decltype(&std::free)> demangled(
            abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status), &std::free);
        out += ' ';
        out += (status == 0 && demangled) ? demangled.get() : info.dli_sname;
        std::snprintf(
            head, sizeof(head), "+0x%lx", static_cast<unsigned long>(pc - static_cast<const char*>(info.dli_saddr)));
        out += head;
    }

    if (found && info.dli_fname != nullptr)
    {
        std::snprintf(
            head, sizeof(head), "+0x%lx)", static_cast<unsigned long>(pc - static_cast<const char*>(info.dli_fbase)));
        out += " (";
        out += info.dli_fname;
        out += head;
    }
}

struct Collector
{
    void** frames;
    std::size_t cap;
    std::size_t n;
};

_Unwind_Reason_Code Visit(_Unwind_Context* ctx, void* arg)
{
    auto* c = static_cast<Collector*>(arg);
    if (c->n >= c->cap)
        return _URC_END_OF_STACK;
    int ip_before_insn = 0;
    const std::uintptr_t ip = _Unwind_GetIPInfo(ctx, &ip_before_insn);
    if (ip == 0)
        return _URC_END_OF_STACK;
    // 统一成"返回地址"口径（AppendFrame 一律按 pc-1 查符号）；信号帧的 ip 本身就指向未执行的那条指令，补 1 对齐。
    c->frames[c->n++] = reinterpret_cast<void*>(ip + (ip_before_insn != 0 ? 1 : 0));
    return _URC_NO_REASON;
}

} // namespace

std::string FormatBacktraceFrame(std::size_t index, const void* return_address)
{
    std::string out;
    AppendFrame(out, index, return_address);
    return out;
}

std::string Backtrace(std::size_t max_depth, std::size_t skip)
{
    if (max_depth == 0 || skip >= kMaxBacktraceFrames)
        return {};

    // +1 跳过 Backtrace 自己这一帧
    const std::size_t skipped = skip + 1;
    void* frames[kMaxBacktraceFrames];
    Collector c{frames, std::min(skipped + max_depth, kMaxBacktraceFrames), 0};
    _Unwind_Backtrace(&Visit, &c);
    if (c.n <= skipped)
        return {};

    const std::size_t end = c.n;
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
