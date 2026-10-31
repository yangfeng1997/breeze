// 无测试框架依赖：失败即 abort。

#include "endpoint.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

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

void TestTcpV4()
{
    const auto ep = Endpoint::Parse("tcp://127.0.0.1:8080");
    CHECK(ep.has_value());
    CHECK(ep->kind() == Endpoint::Kind::Tcp);
    CHECK(ep->address() == "127.0.0.1");
    CHECK(ep->port() == 8080);
    CHECK(!ep->IsIPv6());
    CHECK(ep->ToString() == "tcp://127.0.0.1:8080");
}

void TestTcpV6()
{
    const auto ep = Endpoint::Parse("tcp://[::1]:9");
    CHECK(ep.has_value());
    CHECK(ep->IsIPv6());
    CHECK(ep->port() == 9);
    CHECK(ep->ToString() == "tcp://[::1]:9");
    // 规范化：不同写法的同一地址相等
    CHECK(Endpoint::Parse("tcp://[0:0:0:0:0:0:0:1]:9") == ep);
}

void TestPortRange()
{
    CHECK(Endpoint::Parse("tcp://1.2.3.4:0").has_value()); // 0 = 由系统分配，bind 用
    CHECK(Endpoint::Parse("tcp://1.2.3.4:65535").has_value());
    CHECK(!Endpoint::Parse("tcp://1.2.3.4:65536"));
    CHECK(!Endpoint::Parse("tcp://1.2.3.4:"));
    CHECK(!Endpoint::Parse("tcp://1.2.3.4"));
    CHECK(!Endpoint::Parse("tcp://1.2.3.4:80x"));
    CHECK(!Endpoint::Parse("tcp://1.2.3.4:+80"));
    CHECK(!Endpoint::Parse("tcp://1.2.3.4:-1"));
    CHECK(!Endpoint::Parse("tcp://1.2.3.4:123456"));
}

void TestRejectsNonLiterals()
{
    CHECK(!Endpoint::Parse("tcp://localhost:80")); // 不做 DNS
    CHECK(!Endpoint::Parse("tcp://example.com:80"));
    CHECK(!Endpoint::Parse("tcp://256.1.1.1:80"));
    CHECK(!Endpoint::Parse("tcp://::1:80"));       // v6 必须带方括号，否则有歧义
    CHECK(!Endpoint::Parse("tcp://[1.2.3.4]:80")); // 方括号里必须是 v6
    CHECK(!Endpoint::Parse("tcp://[::1:80"));
    CHECK(!Endpoint::Parse("tcp://:80"));
    // 内嵌 '\0' 不能被 inet_pton 悄悄截断成合法地址
    CHECK(!Endpoint::Tcp(std::string_view("1.2.3.4\0x", 9), 80));
}

void TestUnix()
{
    const auto ep = Endpoint::Parse("unix:///run/breeze/a.sock");
    CHECK(ep.has_value());
    CHECK(ep->kind() == Endpoint::Kind::Unix);
    CHECK(ep->address() == "/run/breeze/a.sock");
    CHECK(ep->ToString() == "unix:///run/breeze/a.sock");

    CHECK(!Endpoint::Parse("unix://relative.sock")); // 必须是绝对路径
    CHECK(!Endpoint::Parse("unix://"));
    CHECK(!Endpoint::Unix(""));
    CHECK(!Endpoint::Unix(std::string_view("/a\0b", 4)));

    // sockaddr_un.sun_path 共 108 字节含结尾，所以路径最长 107
    const std::string p107 = "/" + std::string(106, 'x');
    const std::string p108 = "/" + std::string(107, 'x');
    CHECK(Endpoint::Unix(p107).has_value());
    CHECK(!Endpoint::Unix(p108));
}

void TestUnixAbstract()
{
    const auto ep = Endpoint::Parse("unix-abstract://breeze.sidecar");
    CHECK(ep.has_value());
    CHECK(ep->kind() == Endpoint::Kind::UnixAbstract);
    CHECK(ep->address() == "breeze.sidecar");
    CHECK(ep->ToString() == "unix-abstract://breeze.sidecar");

    CHECK(!Endpoint::Parse("unix-abstract://"));
    CHECK(Endpoint::UnixAbstract(std::string(107, 'a')).has_value());
    CHECK(!Endpoint::UnixAbstract(std::string(108, 'a')));
}

void TestRejectsOtherSchemes()
{
    CHECK(!Endpoint::Parse(""));
    CHECK(!Endpoint::Parse("http://1.2.3.4:80"));
    CHECK(!Endpoint::Parse("1.2.3.4:80"));
    CHECK(!Endpoint::Parse("tcp:/1.2.3.4:80"));
}

// ToString 与 Parse 互逆。
void TestRoundTrip()
{
    for (const char* s : {"tcp://10.0.0.1:1", "tcp://[fe80::1]:65535", "unix:///tmp/x", "unix-abstract://n"})
    {
        const auto ep = Endpoint::Parse(s);
        CHECK(ep.has_value());
        CHECK(Endpoint::Parse(ep->ToString()) == ep);
        CHECK(ep->ToString() == s);
    }
    // 不同端点不相等
    CHECK(Endpoint::Parse("tcp://1.2.3.4:80") != Endpoint::Parse("tcp://1.2.3.4:81"));
    CHECK(Endpoint::Parse("unix:///a") != Endpoint::Parse("unix-abstract:///a"));
}

} // namespace

int main()
{
    TestTcpV4();
    TestTcpV6();
    TestPortRange();
    TestRejectsNonLiterals();
    TestUnix();
    TestUnixAbstract();
    TestRejectsOtherSchemes();
    TestRoundTrip();
    std::puts("endpoint_test: ok");
    return 0;
}
