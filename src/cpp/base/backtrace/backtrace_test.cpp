// Backtrace：行格式、skip 与 max_depth、demangle、模块偏移。
// 符号名要求本可执行文件带 -rdynamic（CMake 里 ENABLE_EXPORTS），且被探测的函数是外部链接。

#include "backtrace.h"

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "assert_test_util.h"

using namespace breeze;

namespace bt_test
{

// 优化器会把"调用后立刻返回"变成尾调用，帧就没了；asm 让结果在返回前"被用过"。
inline void Keep(std::string& s) { asm volatile("" : "+m"(s)); }

[[gnu::noinline]] std::string ProbeInner(std::size_t depth, std::size_t skip)
{
    std::string s = Backtrace(depth, skip);
    Keep(s);
    return s;
}

[[gnu::noinline]] std::string ProbeOuter(std::size_t depth, std::size_t skip)
{
    std::string s = ProbeInner(depth, skip);
    Keep(s);
    return s;
}

[[gnu::noinline]] std::string Recurse(int n, std::size_t depth)
{
    if (n == 0)
        return Backtrace(depth);
    std::string s = Recurse(n - 1, depth);
    Keep(s);
    return s;
}

} // namespace bt_test

namespace
{

std::vector<std::string> Lines(const std::string& s)
{
    std::vector<std::string> out;
    std::istringstream in(s);
    for (std::string line; std::getline(in, line);)
        out.push_back(line);
    return out;
}

void TestLineFormat()
{
    const std::string bt = bt_test::ProbeOuter(8, 0);
    CHECK(!bt.empty());
    CHECK(bt.back() != '\n'); // 帧间换行，末尾不带
    const auto lines = Lines(bt);
    CHECK(!lines.empty() && lines.size() <= 8);
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        const std::string prefix = "#" + std::to_string(i) + " 0x";
        CHECK(lines[i].rfind(prefix, 0) == 0); // 序号从 0 连续
    }
}

void TestSymbolsAndDemangle()
{
    const auto lines = Lines(bt_test::ProbeOuter(8, 0));
    CHECK(lines.size() >= 3);
    if (lines[0].find("bt_test::ProbeInner[abi:cxx11](unsigned long, unsigned long)") == std::string::npos)
        std::fprintf(stderr, "实际输出：\n%s\n", bt_test::ProbeOuter(8, 0).c_str());
    // 栈顶是 Backtrace 的调用方；demangle 过的名字带命名空间与参数表，而不是 _ZN...
    // 返回 std::string 的函数名里带 [abi:cxx11]
    CHECK(lines[0].find("bt_test::ProbeInner[abi:cxx11](unsigned long, unsigned long)") != std::string::npos);
    CHECK(lines[1].find("bt_test::ProbeOuter[abi:cxx11](unsigned long, unsigned long)") != std::string::npos);
    CHECK(lines[0].find("_ZN") == std::string::npos);
    // "+0x" 是函数内偏移
    const auto plus = lines[0].find(")+0x");
    CHECK(plus != std::string::npos);
}

void TestModuleOffset()
{
    const auto lines = Lines(bt_test::ProbeOuter(2, 0));
    CHECK(!lines.empty());
    // 末尾是 (模块+模块内偏移)，可喂给 addr2line
    CHECK(lines[0].find("backtrace_test+0x") != std::string::npos);
    CHECK(lines[0].back() == ')');
}

void TestSkip()
{
    const auto lines = Lines(bt_test::ProbeOuter(8, 1));
    CHECK(!lines.empty());
    CHECK(lines[0].rfind("#0 ", 0) == 0);                             // 序号按输出重新编
    CHECK(lines[0].find("bt_test::ProbeOuter") != std::string::npos); // 跳过了 ProbeInner

    const auto none = bt_test::ProbeOuter(8, 100000); // 跳得比栈深
    CHECK(none.empty());
    CHECK(bt_test::ProbeOuter(8, static_cast<std::size_t>(-1)).empty()); // skip + 1 不能溢出
}

// 返回地址恰好是某个函数的入口：说明调用点是前一个函数的最后一条指令，符号必须按 pc-1 查，不能算到这个函数头上。
void TestReturnAddressAtFunctionEntry()
{
    const void* entry = reinterpret_cast<const void*>(&bt_test::ProbeOuter);
    const std::string at_entry = FormatBacktraceFrame(3, entry);
    CHECK(at_entry.rfind("#3 0x", 0) == 0);
    CHECK(at_entry.find("ProbeOuter") == std::string::npos);

    // 入口之后一个字节还在函数里
    const std::string inside = FormatBacktraceFrame(0, static_cast<const char*>(entry) + 1);
    CHECK(inside.find("bt_test::ProbeOuter") != std::string::npos);
    CHECK(inside.find(")+0x1 (") != std::string::npos);
}

void TestDepthLimit()
{
    CHECK(Backtrace(0).empty());

    const auto ten = Lines(bt_test::Recurse(200, 10));
    CHECK(ten.size() == 10);

    // 要的比上限多：截在 kMaxBacktraceFrames（含 Backtrace 自己那一帧，输出里不显示）
    const auto many = Lines(bt_test::Recurse(300, 100000));
    CHECK(many.size() == kMaxBacktraceFrames - 1);
}

} // namespace

int main()
{
    TestLineFormat();
    TestSymbolsAndDemangle();
    TestModuleOffset();
    TestSkip();
    TestReturnAddressAtFunctionEntry();
    TestDepthLimit();
    std::puts("backtrace_test: ok");
    return 0;
}
