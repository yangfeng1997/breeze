// socket_address：Endpoint 与 sockaddr 互转、按协议族建 socket、数据报（UDP）的一来一回。
// 这是 rudp 要用的地基：UDP 的 sendto / recvfrom 靠它。
// 无测试框架依赖：失败即 abort。

#include "socket_address.h"

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

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

int GetSockInt(int fd, int name)
{
    int v = -1;
    socklen_t len = sizeof(v);
    CHECK(::getsockopt(fd, SOL_SOCKET, name, &v, &len) == 0);
    return v;
}

void WaitReadable(int fd)
{
    pollfd p{fd, POLLIN, 0};
    CHECK(::poll(&p, 1, 5000) > 0);
}

// 互转：Endpoint → sockaddr → Endpoint 回到原样（IP 类端点按指定的种类）。
void TestRoundTripAllKinds()
{
    for (const char* text : {"tcp://127.0.0.1:8080", "tcp://[::1]:9", "udp://10.1.2.3:65535", "udp://[fe80::1]:1"})
    {
        const auto ep = Endpoint::Parse(text);
        CHECK(ep.has_value());
        sockaddr_storage ss;
        const socklen_t len = sock::ToSockaddr(*ep, ss);
        CHECK(len == (ep->IsIPv6() ? sizeof(sockaddr_in6) : sizeof(sockaddr_in)));
        CHECK(ss.ss_family == (ep->IsIPv6() ? AF_INET6 : AF_INET));
        const auto back = sock::FromSockaddr(ss, len, ep->kind());
        CHECK(back.has_value() && *back == *ep);
    }
    for (const char* text : {"unix:///tmp/a.sock", "unix-abstract://breeze-addr-test"})
    {
        const auto ep = Endpoint::Parse(text);
        CHECK(ep.has_value());
        sockaddr_storage ss;
        const socklen_t len = sock::ToSockaddr(*ep, ss);
        CHECK(ss.ss_family == AF_UNIX);
        const auto back = sock::FromSockaddr(ss, len);
        CHECK(back.has_value() && *back == *ep);
    }
}

// FromSockaddr 的 ip_kind：同一个 sockaddr，字节流默认得到 Tcp，数据报传 Udp 得到 Udp。
void TestFromSockaddrKind()
{
    sockaddr_storage ss;
    const socklen_t len = sock::ToSockaddr(*Endpoint::Tcp("1.2.3.4", 80), ss);
    CHECK(sock::FromSockaddr(ss, len)->kind() == Endpoint::Kind::Tcp);
    CHECK(sock::FromSockaddr(ss, len, Endpoint::Kind::Tcp)->kind() == Endpoint::Kind::Tcp);
    CHECK(sock::FromSockaddr(ss, len, Endpoint::Kind::Udp)->kind() == Endpoint::Kind::Udp);
    CHECK(*sock::FromSockaddr(ss, len, Endpoint::Kind::Udp) == *Endpoint::Udp("1.2.3.4", 80));
    // UDS 的地址不看 ip_kind
    sockaddr_storage us;
    const socklen_t ulen = sock::ToSockaddr(*Endpoint::Unix("/tmp/k.sock"), us);
    CHECK(sock::FromSockaddr(us, ulen, Endpoint::Kind::Udp)->kind() == Endpoint::Kind::Unix);
}

void TestFromSockaddrRejects()
{
    sockaddr_storage ss{};
    ss.ss_family = AF_UNSPEC;
    CHECK(!sock::FromSockaddr(ss, sizeof(ss)).has_value()); // 地址族不认识
    sockaddr_storage us{};
    us.ss_family = AF_UNIX;
    CHECK(!sock::FromSockaddr(us, sizeof(sa_family_t)).has_value()); // 未命名的 UDS 对端
}

// 按端点的协议族与类型建 socket：IP 类端点是 AF_INET / AF_INET6，UDS 是 AF_UNIX；Udp 端点也是 IP 类。
void TestCreateSocketDomainAndType()
{
    struct Case
    {
        const char* text;
        int domain;
        int type;
    };
    for (const Case& c : {Case{"tcp://127.0.0.1:0", AF_INET, SOCK_STREAM},
                          Case{"tcp://[::1]:0", AF_INET6, SOCK_STREAM},
                          Case{"udp://127.0.0.1:0", AF_INET, SOCK_DGRAM},
                          Case{"udp://[::1]:0", AF_INET6, SOCK_DGRAM},
                          Case{"unix:///tmp/x.sock", AF_UNIX, SOCK_STREAM},
                          Case{"unix-abstract://x", AF_UNIX, SOCK_DGRAM}})
    {
        const auto ep = Endpoint::Parse(c.text);
        CHECK(ep.has_value());
        UniqueFd fd;
        const int rc = sock::CreateSocket(*ep, c.type, fd);
        if (rc == EAFNOSUPPORT) // 本机没开 IPv6
            continue;
        CHECK(rc == 0 && fd.valid());
        CHECK(GetSockInt(fd.get(), SO_DOMAIN) == c.domain);
        CHECK(GetSockInt(fd.get(), SO_TYPE) == c.type);
    }
}

// UDP 一来一回：绑定端口 0 → getsockname 取回实际端口（Udp 种类）→ 另一个 socket sendto → recvfrom 看到对端地址。
void TestUdpLoopbackExchange()
{
    const auto any = Endpoint::Udp("127.0.0.1", 0);
    CHECK(any.has_value());
    UniqueFd server;
    CHECK(sock::CreateSocket(*any, SOCK_DGRAM, server) == 0);
    sockaddr_storage ss;
    socklen_t len = sock::ToSockaddr(*any, ss);
    CHECK(::bind(server.get(), reinterpret_cast<const sockaddr*>(&ss), len) == 0);

    sockaddr_storage bound{};
    socklen_t blen = sizeof(bound);
    CHECK(::getsockname(server.get(), reinterpret_cast<sockaddr*>(&bound), &blen) == 0);
    const auto server_ep = sock::FromSockaddr(bound, blen, Endpoint::Kind::Udp);
    CHECK(server_ep.has_value() && server_ep->kind() == Endpoint::Kind::Udp && server_ep->port() != 0);

    UniqueFd client;
    CHECK(sock::CreateSocket(*any, SOCK_DGRAM, client) == 0);
    len = sock::ToSockaddr(*server_ep, ss);
    CHECK(::sendto(client.get(), "ping", 4, 0, reinterpret_cast<const sockaddr*>(&ss), len) == 4);

    WaitReadable(server.get());
    char buf[16];
    sockaddr_storage from{};
    socklen_t flen = sizeof(from);
    const ssize_t n = ::recvfrom(server.get(), buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &flen);
    CHECK(n == 4 && std::memcmp(buf, "ping", 4) == 0);
    const auto peer = sock::FromSockaddr(from, flen, Endpoint::Kind::Udp); // 对端地址按 Udp 种类取回
    CHECK(peer.has_value() && peer->kind() == Endpoint::Kind::Udp && peer->address() == "127.0.0.1");

    // 用取回的对端地址回一个包
    len = sock::ToSockaddr(*peer, ss);
    CHECK(::sendto(server.get(), "pong", 4, 0, reinterpret_cast<const sockaddr*>(&ss), len) == 4);
    WaitReadable(client.get());
    CHECK(::recv(client.get(), buf, sizeof(buf), 0) == 4 && std::memcmp(buf, "pong", 4) == 0);
}

} // namespace

int main()
{
    TestRoundTripAllKinds();
    TestFromSockaddrKind();
    TestFromSockaddrRejects();
    TestCreateSocketDomainAndType();
    TestUdpLoopbackExchange();
    std::puts("socket_address_test: ok");
    return 0;
}
