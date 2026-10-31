// RudpEndpoint：构造与访问器、解析、从通用 Endpoint 转换（种类不对就失败）、与通用形式互通；与 Tcp / Uds 端点互不接受。
// 无测试框架依赖：失败即 abort。

#include "rudp_endpoint.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>

#include "tcp_endpoint.h"
#include "uds_endpoint.h"

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
static_assert(!std::is_default_constructible_v<RudpEndpoint> && !std::is_constructible_v<RudpEndpoint, Endpoint> &&
              !std::is_convertible_v<Endpoint, RudpEndpoint>);
static_assert(!std::is_convertible_v<TcpEndpoint, RudpEndpoint> && !std::is_convertible_v<RudpEndpoint, TcpEndpoint> &&
              !std::is_convertible_v<UdsEndpoint, RudpEndpoint> && !std::is_convertible_v<RudpEndpoint, UdsEndpoint>);

namespace
{

void TestMakeAndAccessors()
{
    const auto v4 = RudpEndpoint::Make("127.0.0.1", 8080);
    CHECK(v4.has_value());
    CHECK(v4->ip() == "127.0.0.1" && v4->port() == 8080 && !v4->IsIPv6());
    CHECK(v4->ToString() == "udp://127.0.0.1:8080");
    CHECK(v4->endpoint().kind() == Endpoint::Kind::Udp);

    const auto v6 = RudpEndpoint::Make("::1", 9);
    CHECK(v6.has_value() && v6->IsIPv6() && v6->ToString() == "udp://[::1]:9");
    CHECK(RudpEndpoint::Make("0:0:0:0:0:0:0:1", 9) == v6); // IP 规范化后保存

    CHECK(RudpEndpoint::Make("0.0.0.0", 0).has_value()); // 端口 0：由系统分配，bind 用
    for (const char* bad : {"localhost", "256.1.1.1", "1.2.3.4 ", "", "1.2.3", "::g", "udp://1.2.3.4"})
        CHECK(!RudpEndpoint::Make(bad, 80).has_value());
}

void TestParse()
{
    const auto a = RudpEndpoint::Parse("udp://1.2.3.4:80");
    CHECK(a.has_value() && a->ip() == "1.2.3.4" && a->port() == 80);
    const auto b = RudpEndpoint::Parse("udp://[::1]:80");
    CHECK(b.has_value() && b->IsIPv6() && b->port() == 80);

    // 别的传输的写法、缺方括号的 v6、不认识的 scheme、空串：都得到 nullopt
    for (const char* bad : {"udp://::1:80",
                            "tcp://1.2.3.4:80",
                            "unix:///a.sock",
                            "unix-abstract://x",
                            "rudp://1.2.3.4:80",
                            "",
                            "udp://",
                            "1.2.3.4:80"})
        CHECK(!RudpEndpoint::Parse(bad).has_value());

    // 与 ToString 互逆
    CHECK(RudpEndpoint::Parse(a->ToString()) == a && RudpEndpoint::Parse(b->ToString()) == b);
}

void TestFromGenericEndpoint()
{
    const auto g = Endpoint::Udp("10.0.0.1", 7000);
    CHECK(g.has_value());
    const auto r = RudpEndpoint::From(*g);
    CHECK(r.has_value() && r->endpoint() == *g && r->port() == 7000);
    CHECK(RudpEndpoint::Make("10.0.0.1", 7000) == r);

    // 同样的地址与端口，TCP 的转不了；UDS 的也转不了
    CHECK(!RudpEndpoint::From(*Endpoint::Tcp("10.0.0.1", 7000)).has_value());
    CHECK(!RudpEndpoint::From(*Endpoint::Unix("/tmp/a.sock")).has_value());
    CHECK(!RudpEndpoint::From(*Endpoint::UnixAbstract("a")).has_value());
}

// 三种类型化端点互不接受对方：同一个通用 Endpoint 只会被其中一个认领。
void TestTypedEndpointsAreDisjoint()
{
    const Endpoint eps[] = {*Endpoint::Tcp("1.2.3.4", 80),
                            *Endpoint::Udp("1.2.3.4", 80),
                            *Endpoint::Unix("/a"),
                            *Endpoint::UnixAbstract("a")};
    for (const Endpoint& ep : eps)
    {
        const int claimed = static_cast<int>(TcpEndpoint::From(ep).has_value()) +
                            static_cast<int>(RudpEndpoint::From(ep).has_value()) +
                            static_cast<int>(UdsEndpoint::From(ep).has_value());
        CHECK(claimed == 1);
    }
}

void TestEquality()
{
    CHECK(RudpEndpoint::Make("1.2.3.4", 80) == RudpEndpoint::Make("1.2.3.4", 80));
    CHECK(RudpEndpoint::Make("1.2.3.4", 80) != RudpEndpoint::Make("1.2.3.4", 81));
    CHECK(RudpEndpoint::Make("1.2.3.4", 80) != RudpEndpoint::Make("1.2.3.5", 80));
}

} // namespace

int main()
{
    TestMakeAndAccessors();
    TestParse();
    TestFromGenericEndpoint();
    TestTypedEndpointsAreDisjoint();
    TestEquality();
    std::puts("rudp_endpoint_test: ok");
    return 0;
}
