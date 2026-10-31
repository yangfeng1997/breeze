// ConfigureSocket：TCP 连接按 TcpOptions 设 NODELAY / keepalive / TCP_USER_TIMEOUT，UDS 连接取对端的 credentials 且
// 不碰 TCP 选项。socket_test 测的是各个函数本身，这里测的是"按类型分派"的那一层。
// 无测试框架依赖：失败即 abort。

#include "socket_config.h"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

#include "socket.h"
#include "socket_address.h"

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

int GetIntOpt(int fd, int level, int name)
{
    int v = -1;
    socklen_t len = sizeof(v);
    CHECK(::getsockopt(fd, level, name, &v, &len) == 0);
    return v;
}

void WaitFor(int fd, short events)
{
    pollfd p{fd, events, 0};
    CHECK(::poll(&p, 1, 5000) > 0);
}

// 连好一对 socket，返回服务端 accept 到的那一端（客户端那端也要活着，所以一并带回去）。
struct Pair
{
    UniqueFd client;
    UniqueFd server;
};

Pair Connected(const Endpoint& listen_ep)
{
    UniqueFd lfd;
    CHECK(sock::CreateStreamSocket(listen_ep, lfd) == 0);
    CHECK(sock::Bind(lfd.get(), listen_ep) == 0);
    CHECK(sock::Listen(lfd.get(), 4) == 0);
    std::optional<Endpoint> local;
    CHECK(sock::LocalEndpoint(lfd.get(), local) == 0 && local.has_value());

    Pair p;
    CHECK(sock::CreateStreamSocket(*local, p.client) == 0);
    const int rc = sock::Connect(p.client.get(), *local);
    CHECK(rc == 0 || rc == EINPROGRESS);
    WaitFor(p.client.get(), POLLOUT);
    CHECK(sock::GetSocketError(p.client.get()) == 0);
    WaitFor(lfd.get(), POLLIN);
    std::optional<Endpoint> peer;
    CHECK(sock::Accept(lfd.get(), p.server, peer) == 0);
    return p;
}

void TestTcpAppliesAllOptions()
{
    const Pair p = Connected(*Endpoint::Tcp("127.0.0.1", 0));
    TcpOptions tcp;
    tcp.keepalive_idle = std::chrono::seconds(30);
    tcp.keepalive_interval = std::chrono::seconds(5);
    tcp.keepalive_count = 2;
    tcp.tcp_user_timeout = std::chrono::milliseconds(20000);
    PeerInfo info;
    ConfigureSocket(p.server.get(), Endpoint::Kind::Tcp, tcp, info);

    const int fd = p.server.get();
    CHECK(GetIntOpt(fd, IPPROTO_TCP, TCP_NODELAY) == 1);
    CHECK(GetIntOpt(fd, SOL_SOCKET, SO_KEEPALIVE) == 1);
    CHECK(GetIntOpt(fd, IPPROTO_TCP, TCP_KEEPIDLE) == 30);
    CHECK(GetIntOpt(fd, IPPROTO_TCP, TCP_KEEPINTVL) == 5);
    CHECK(GetIntOpt(fd, IPPROTO_TCP, TCP_KEEPCNT) == 2);
    CHECK(GetIntOpt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT) == 20000);
    CHECK(!info.credentials.has_value()); // credentials 是 UDS 才有的
}

void TestTcpDefaultsTurnKeepAliveOn()
{
    const Pair p = Connected(*Endpoint::Tcp("127.0.0.1", 0));
    PeerInfo info;
    ConfigureSocket(p.server.get(), Endpoint::Kind::Tcp, TcpOptions{}, info);
    CHECK(GetIntOpt(p.server.get(), IPPROTO_TCP, TCP_NODELAY) == 1);
    CHECK(GetIntOpt(p.server.get(), SOL_SOCKET, SO_KEEPALIVE) == 1); // 默认约 90s 发现对端消失
    CHECK(GetIntOpt(p.server.get(), IPPROTO_TCP, TCP_KEEPIDLE) == 60);
    CHECK(GetIntOpt(p.server.get(), IPPROTO_TCP, TCP_KEEPINTVL) == 10);
    CHECK(GetIntOpt(p.server.get(), IPPROTO_TCP, TCP_KEEPCNT) == 3);
    CHECK(GetIntOpt(p.server.get(), IPPROTO_TCP, TCP_USER_TIMEOUT) == 0);
}

// keepalive_idle 为 0 = 不开 keepalive；TCP_USER_TIMEOUT 仍然可以单独设。
void TestTcpKeepAliveOffUserTimeoutAlone()
{
    const Pair p = Connected(*Endpoint::Tcp("127.0.0.1", 0));
    TcpOptions tcp;
    tcp.keepalive_idle = std::chrono::seconds(0);
    tcp.tcp_user_timeout = std::chrono::milliseconds(15000);
    PeerInfo info;
    ConfigureSocket(p.server.get(), Endpoint::Kind::Tcp, tcp, info);
    CHECK(GetIntOpt(p.server.get(), SOL_SOCKET, SO_KEEPALIVE) == 0);
    CHECK(GetIntOpt(p.server.get(), IPPROTO_TCP, TCP_USER_TIMEOUT) == 15000);
    CHECK(GetIntOpt(p.server.get(), IPPROTO_TCP, TCP_NODELAY) == 1);
}

void TestTcpNothingOptionalSet()
{
    const Pair p = Connected(*Endpoint::Tcp("127.0.0.1", 0));
    TcpOptions tcp;
    tcp.keepalive_idle = std::chrono::seconds(0);
    PeerInfo info;
    ConfigureSocket(p.server.get(), Endpoint::Kind::Tcp, tcp, info);
    CHECK(GetIntOpt(p.server.get(), SOL_SOCKET, SO_KEEPALIVE) == 0);
    CHECK(GetIntOpt(p.server.get(), IPPROTO_TCP, TCP_USER_TIMEOUT) == 0);
    CHECK(GetIntOpt(p.server.get(), IPPROTO_TCP, TCP_NODELAY) == 1);
}

// UDS：取对端 credentials，不去设 TCP 选项（在 UDS 上设会失败，也不该尝试）。
void TestUdsFillsCredentials()
{
    const auto ep = Endpoint::UnixAbstract("breeze-socket-config-test-" + std::to_string(::getpid()));
    CHECK(ep.has_value());
    const Pair p = Connected(*ep);
    PeerInfo info;
    ConfigureSocket(p.server.get(), Endpoint::Kind::UnixAbstract, TcpOptions{}, info);
    CHECK(info.credentials.has_value());
    CHECK(info.credentials->pid == ::getpid() && info.credentials->uid == ::getuid() &&
          info.credentials->gid == ::getgid());
    CHECK(GetIntOpt(p.server.get(), SOL_SOCKET, SO_KEEPALIVE) == 0); // 没被设过
}

// UDP 端点：数据报没有"连接"，ConfigureSocket 什么都不做（尤其不能落到 UDS 那一支去取 credentials）。
void TestUdpIsNoOp()
{
    UniqueFd fd;
    CHECK(sock::CreateSocket(*Endpoint::Udp("127.0.0.1", 0), SOCK_DGRAM, fd) == 0);
    PeerInfo info;
    ConfigureSocket(fd.get(), Endpoint::Kind::Udp, TcpOptions{}, info);
    CHECK(!info.credentials.has_value());
    CHECK(GetIntOpt(fd.get(), SOL_SOCKET, SO_KEEPALIVE) == 0);
}

} // namespace

int main()
{
    TestTcpAppliesAllOptions();
    TestTcpDefaultsTurnKeepAliveOn();
    TestTcpKeepAliveOffUserTimeoutAlone();
    TestTcpNothingOptionalSet();
    TestUdsFillsCredentials();
    TestUdpIsNoOp();
    std::puts("socket_config_test: ok");
    return 0;
}
