// UdsEndpoint：路径 / 抽象名的构造与访问器、解析、从通用 Endpoint 转换（种类不对就失败）、与通用形式互通。
// 无测试框架依赖：失败即 abort。

#include "uds_endpoint.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <type_traits>

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

// 只能经工厂函数拿到，且不能从通用 Endpoint 隐式构造（要走 From，种类不对会失败）。
static_assert(!std::is_default_constructible_v<UdsEndpoint> && !std::is_constructible_v<UdsEndpoint, Endpoint> &&
              !std::is_convertible_v<Endpoint, UdsEndpoint>);

namespace
{

void TestPathAndAbstract()
{
    const auto p = UdsEndpoint::Path("/run/breeze/x.sock");
    CHECK(p.has_value() && !p->is_abstract() && p->address() == "/run/breeze/x.sock");
    CHECK(p->ToString() == "unix:///run/breeze/x.sock");

    const auto a = UdsEndpoint::Abstract("breeze-x");
    CHECK(a.has_value() && a->is_abstract() && a->address() == "breeze-x");
    CHECK(a->ToString() == "unix-abstract://breeze-x");

    CHECK(p != a);
}

void TestConstraints()
{
    // 路径必须是绝对路径、不能为空、不能含 NUL、长度 ≤107（sun_path 共 108 字节，含结尾）
    CHECK(!UdsEndpoint::Path("relative.sock").has_value());
    CHECK(!UdsEndpoint::Path("").has_value());
    CHECK(!UdsEndpoint::Path(std::string_view("/a\0b", 4)).has_value());
    CHECK(UdsEndpoint::Path("/" + std::string(106, 'a')).has_value());
    CHECK(!UdsEndpoint::Path("/" + std::string(107, 'a')).has_value());

    CHECK(!UdsEndpoint::Abstract("").has_value());
    CHECK(!UdsEndpoint::Abstract(std::string_view("a\0b", 3)).has_value());
    CHECK(UdsEndpoint::Abstract(std::string(107, 'a')).has_value());
    CHECK(!UdsEndpoint::Abstract(std::string(108, 'a')).has_value());
}

void TestParse()
{
    const auto p = UdsEndpoint::Parse("unix:///tmp/a.sock");
    CHECK(p.has_value() && !p->is_abstract() && p->address() == "/tmp/a.sock");
    const auto a = UdsEndpoint::Parse("unix-abstract://name");
    CHECK(a.has_value() && a->is_abstract() && a->address() == "name");

    // TCP 的写法、相对路径、不认识的 scheme、空串：都得到 nullopt
    for (const char* bad : {"tcp://127.0.0.1:80",
                            "udp://127.0.0.1:80",
                            "unix://relative",
                            "unix://",
                            "unix-abstract://",
                            "file:///a",
                            ""})
        CHECK(!UdsEndpoint::Parse(bad).has_value());

    // 与 ToString 互逆
    CHECK(UdsEndpoint::Parse(p->ToString()) == p && UdsEndpoint::Parse(a->ToString()) == a);
}

void TestFromGenericEndpoint()
{
    const auto gp = Endpoint::Unix("/tmp/b.sock");
    const auto ga = Endpoint::UnixAbstract("b");
    CHECK(gp.has_value() && ga.has_value());
    const auto p = UdsEndpoint::From(*gp);
    const auto a = UdsEndpoint::From(*ga);
    CHECK(p.has_value() && p->endpoint() == *gp && UdsEndpoint::Path("/tmp/b.sock") == p);
    CHECK(a.has_value() && a->endpoint() == *ga && UdsEndpoint::Abstract("b") == a);

    // TCP 的端点转不了
    CHECK(!UdsEndpoint::From(*Endpoint::Tcp("127.0.0.1", 80)).has_value());
    CHECK(!UdsEndpoint::From(*Endpoint::Udp("127.0.0.1", 80)).has_value()); // 以前"不是 TCP 就是 UDS"，UDP 会漏进来
}

} // namespace

int main()
{
    TestPathAndAbstract();
    TestConstraints();
    TestParse();
    TestFromGenericEndpoint();
    std::puts("uds_endpoint_test: ok");
    return 0;
}
