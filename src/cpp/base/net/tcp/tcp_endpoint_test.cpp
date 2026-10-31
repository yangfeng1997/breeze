// TcpEndpoint：构造与访问器、解析、从通用 Endpoint 转换（种类不对就失败）、与通用形式互通。
// 无测试框架依赖：失败即 abort。

#include "tcp_endpoint.h"

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
static_assert(!std::is_default_constructible_v<TcpEndpoint> && !std::is_constructible_v<TcpEndpoint, Endpoint> &&
              !std::is_convertible_v<Endpoint, TcpEndpoint>);

namespace
{

void TestMakeAndAccessors()
{
    const auto v4 = TcpEndpoint::Make("127.0.0.1", 8080);
    CHECK(v4.has_value());
    CHECK(v4->ip() == "127.0.0.1" && v4->port() == 8080 && !v4->IsIPv6());
    CHECK(v4->ToString() == "tcp://127.0.0.1:8080");

    const auto v6 = TcpEndpoint::Make("::1", 9);
    CHECK(v6.has_value() && v6->IsIPv6() && v6->ToString() == "tcp://[::1]:9");
    CHECK(TcpEndpoint::Make("0:0:0:0:0:0:0:1", 9) == v6); // IP 规范化后保存

    CHECK(TcpEndpoint::Make("0.0.0.0", 0).has_value()); // 端口 0：由系统分配，bind 用

    // 只接 IP 字面量：主机名、越界、带多余字符、空串都不行
    for (const char* bad : {"localhost", "256.1.1.1", "1.2.3.4 ", "", "1.2.3", "::g", "unix:///a"})
        CHECK(!TcpEndpoint::Make(bad, 80).has_value());
}

void TestParse()
{
    const auto a = TcpEndpoint::Parse("tcp://1.2.3.4:80");
    CHECK(a.has_value() && a->ip() == "1.2.3.4" && a->port() == 80);
    const auto b = TcpEndpoint::Parse("tcp://[::1]:80");
    CHECK(b.has_value() && b->IsIPv6() && b->port() == 80);

    // 别的传输的写法、缺方括号的 v6、不认识的 scheme、空串：都得到 nullopt
    for (const char* bad :
         {"tcp://::1:80", "unix:///a.sock", "unix-abstract://x", "http://1.2.3.4:80", "", "tcp://", "1.2.3.4:80"})
        CHECK(!TcpEndpoint::Parse(bad).has_value());

    // 与 ToString 互逆
    CHECK(TcpEndpoint::Parse(a->ToString()) == a && TcpEndpoint::Parse(b->ToString()) == b);
}

void TestFromGenericEndpoint()
{
    const auto g = Endpoint::Tcp("10.0.0.1", 7000);
    CHECK(g.has_value());
    const auto t = TcpEndpoint::From(*g);
    CHECK(t.has_value() && t->endpoint() == *g && t->port() == 7000);
    CHECK(TcpEndpoint::Make("10.0.0.1", 7000) == t);

    // UDS 的端点转不了
    CHECK(!TcpEndpoint::From(*Endpoint::Unix("/tmp/a.sock")).has_value());
    CHECK(!TcpEndpoint::From(*Endpoint::UnixAbstract("a")).has_value());
}

void TestEquality()
{
    CHECK(TcpEndpoint::Make("1.2.3.4", 80) == TcpEndpoint::Make("1.2.3.4", 80));
    CHECK(TcpEndpoint::Make("1.2.3.4", 80) != TcpEndpoint::Make("1.2.3.4", 81));
    CHECK(TcpEndpoint::Make("1.2.3.4", 80) != TcpEndpoint::Make("1.2.3.5", 80));
}

} // namespace

int main()
{
    TestMakeAndAccessors();
    TestParse();
    TestFromGenericEndpoint();
    TestEquality();
    std::puts("tcp_endpoint_test: ok");
    return 0;
}
