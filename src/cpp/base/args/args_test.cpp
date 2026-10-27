// 无测试框架依赖：失败即 abort。--help 与错误用例会向 stdout / stderr 打印帮助与原因，属预期输出。

#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

#include "args_parser.h"

using namespace breeze;

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

namespace
{

ParseResult Run(ArgsParser& p, std::initializer_list<const char*> args)
{
    std::vector<const char*> argv{"prog"};
    argv.insert(argv.end(), args.begin(), args.end());
    return p.Parse(static_cast<int>(argv.size()), argv.data());
}

void TestParseAll()
{
    std::string cfg;
    int world = 0;
    std::int64_t big = 0;
    std::uint32_t id = 0;
    double ratio = 0;
    bool verbose = false;
    ArgsParser p("t");
    p.Add("-c,--config", cfg, "配置路径", kRequired);
    p.Add("--world-id", world, "世界 ID");
    p.Add("--big", big, "");
    p.Add("--id", id, "");
    p.Add("--ratio", ratio, "");
    p.AddFlag("--verbose", verbose, "");

    auto r =
        Run(p, {"-c", "a.yaml", "--world-id=2", "--big", "9000000000", "--id", "7", "--ratio", "0.5", "--verbose"});
    CHECK(r.proceed && r.exitCode == 0);
    CHECK(cfg == "a.yaml" && world == 2 && big == 9000000000 && id == 7 && ratio == 0.5);
    CHECK(verbose);
}

void TestDefaultsKept()
{
    std::string cfg = "default.yaml";
    int world = 5;
    bool verbose = false;
    ArgsParser p("t");
    p.Add("--config", cfg, "");
    p.Add("--world-id", world, "");
    p.AddFlag("--verbose", verbose, "");

    auto r = Run(p, {});
    CHECK(r.proceed);
    CHECK(cfg == "default.yaml" && world == 5 && !verbose);
}

void TestMissingRequired()
{
    std::string cfg;
    ArgsParser p("t");
    p.Add("--config", cfg, "", kRequired);

    auto r = Run(p, {});
    CHECK(!r.proceed && r.exitCode != 0);
}

void TestBadValueAndUnknownOption()
{
    int world = 0;
    ArgsParser p("t");
    p.Add("--world-id", world, "");
    CHECK(!Run(p, {"--world-id", "abc"}).proceed);

    ArgsParser q("t");
    CHECK(!Run(q, {"--nope"}).proceed);
}

void TestHelpExitsZero()
{
    ArgsParser p("t");
    auto r = Run(p, {"--help"});
    CHECK(!r.proceed && r.exitCode == 0);
}

void TestDuplicateNameThrows()
{
    int a = 0, b = 0;
    ArgsParser p("t");
    p.Add("--x", a, "");
    bool threw = false;
    try
    {
        p.Add("--x", b, "");
    }
    catch (const std::logic_error&)
    {
        threw = true;
    }
    CHECK(threw);
}

} // namespace

int main()
{
    TestParseAll();
    TestDefaultsKept();
    TestMissingRequired();
    TestBadValueAndUnknownOption();
    TestHelpExitsZero();
    TestDuplicateNameThrows();
    std::puts("args_test: ok");
}
