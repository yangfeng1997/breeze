// 防御式断言的 Debug 路径（BREEZE_ASSERT_FATAL=1）：五个宏失败都 abort，并留下表达式、位置、调用栈。
// 每个用例 fork 一个子进程，所以跑到这里时进程里不能有别的线程。

#define BREEZE_ASSERT_FATAL 1
#include <cstdio>
#include <string>

#include "base/assert/assert_macros.h"
#include "base/assert/assert_test_util.h"

using breeze::testing::ExpectAbort;

// 外部链接 + noinline：-rdynamic 下栈里能看到它的名字，用来断言"栈顶是宏所在的函数"。
// （宏写在 lambda 里不行：Release 下 lambda 会被内联掉，Debug 下它的 operator() 又没有动态符号。）
[[gnu::noinline]] int FatalProbeVal(int x)
{
    ASSERT_RET_VAL(x == 1, 0);
    return 1;
}

[[gnu::noinline]] void FatalProbe(int which, int x)
{
    switch (which)
    {
        case 0:
            ASSERT(x == 1);
            break;
        case 1:
            ASSERT_RET_VOID(x == 1);
            break;
        case 2:
            FatalProbeVal(x);
            break;
        case 3:
            for (int i = 0; i < 1; ++i)
            {
                ASSERT_CONTINUE(x == 1);
            }
            break;
        case 4:
            for (int i = 0; i < 1; ++i)
            {
                ASSERT_BREAK(x == 1);
            }
            break;
        default:
            break;
    }
}

namespace
{

void TestPassDoesNotAbort()
{
    for (int which = 0; which < 5; ++which)
        FatalProbe(which, 1); // x == 1，全部通过
}

void TestEveryMacroAborts()
{
    for (int which = 0; which < 5; ++which)
    {
        std::string out;
        CHECK(ExpectAbort([which] { FatalProbe(which, 2); }, &out));
        CHECK(out.find("ASSERT failed: (x == 1)") != std::string::npos);
        CHECK(out.find("assert_fatal_test.cpp") != std::string::npos);
        CHECK(out.find("backtrace:") != std::string::npos);
        // 栈顶是宏所在的函数，不是断言库自己的帧
        const auto bt = out.find("#0 0x");
        CHECK(bt != std::string::npos);
        const std::string top = out.substr(bt, out.find('\n', bt) - bt);
        const char* expect = which == 2 ? "FatalProbeVal(int)" : "FatalProbe(int, int)";
        if (top.find(expect) == std::string::npos)
            std::fprintf(stderr, "which=%d 子进程的 stderr：\n%s\n", which, out.c_str());
        CHECK(top.find(expect) != std::string::npos);
        CHECK(out.find("AssertAbort") == std::string::npos);
    }
}

} // namespace

int main()
{
    TestPassDoesNotAbort();
    TestEveryMacroAborts();
    std::puts("assert_fatal_test: ok");
    return 0;
}
