// 无测试框架依赖：失败即 abort。全部走本机回环与临时目录里的 UDS，不需要外部环境。

#include "socket.h"

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "tcp_socket.h"
#include "uds_socket.h"

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

constexpr int kWaitMs = 5000;

bool WaitFor(int fd, short events)
{
    pollfd p{fd, events, 0};
    return ::poll(&p, 1, kWaitMs) > 0;
}

int GetIntOpt(int fd, int level, int name)
{
    int v = -1;
    socklen_t len = sizeof(v);
    CHECK(::getsockopt(fd, level, name, &v, &len) == 0);
    return v;
}

// 建好一个监听 socket。
UniqueFd Listener(const Endpoint& ep, int expect_bind_errno = 0)
{
    UniqueFd fd;
    CHECK(sock::CreateStreamSocket(ep, fd) == 0);
    CHECK(tcp::SetReuseAddr(fd.get()) == 0 || ep.kind() != Endpoint::Kind::Tcp);
    const int rc = sock::Bind(fd.get(), ep);
    CHECK(rc == expect_bind_errno);
    if (rc == 0)
        CHECK(sock::Listen(fd.get(), 16) == 0);
    return fd;
}

// 客户端连上 server_ep，返回 {客户端 fd, 服务端接受到的 fd}。
struct Pair
{
    UniqueFd client;
    UniqueFd server;
    std::optional<Endpoint> peer; // 服务端看到的对端地址
};

Pair ConnectAndAccept(const UniqueFd& listener, const Endpoint& server_ep)
{
    Pair p;
    CHECK(sock::CreateStreamSocket(server_ep, p.client) == 0);
    const int rc = sock::Connect(p.client.get(), server_ep);
    CHECK(rc == 0 || rc == EINPROGRESS);
    CHECK(WaitFor(p.client.get(), POLLOUT));
    CHECK(sock::GetSocketError(p.client.get()) == 0);
    CHECK(WaitFor(listener.get(), POLLIN));
    CHECK(sock::Accept(listener.get(), p.server, p.peer) == 0);
    CHECK(p.server.valid());
    return p;
}

void TestTcpLoopback()
{
    const auto any = Endpoint::Tcp("127.0.0.1", 0);
    CHECK(any.has_value());
    const UniqueFd lfd = Listener(*any);

    std::optional<Endpoint> local;
    CHECK(sock::LocalEndpoint(lfd.get(), local) == 0);
    CHECK(local.has_value() && local->port() != 0); // 系统分配了端口

    UniqueFd none;
    std::optional<Endpoint> peer;
    CHECK(sock::Accept(lfd.get(), none, peer) == EAGAIN); // 没有待接受的连接
    CHECK(!none.valid());

    Pair p = ConnectAndAccept(lfd, *local);
    CHECK(p.peer.has_value() && p.peer->address() == "127.0.0.1");
    CHECK(!tcp::IsSelfConnect(p.client.get()));

    // 选项
    CHECK(tcp::SetNoDelay(p.client.get()) == 0);
    CHECK(GetIntOpt(p.client.get(), IPPROTO_TCP, TCP_NODELAY) == 1);
    const tcp::KeepAlive ka{30, 5, 2, 20000};
    CHECK(tcp::SetKeepAlive(p.client.get(), ka) == 0);
    CHECK(GetIntOpt(p.client.get(), SOL_SOCKET, SO_KEEPALIVE) == 1);
    CHECK(GetIntOpt(p.client.get(), IPPROTO_TCP, TCP_KEEPIDLE) == 30);
    CHECK(GetIntOpt(p.client.get(), IPPROTO_TCP, TCP_KEEPINTVL) == 5);
    CHECK(GetIntOpt(p.client.get(), IPPROTO_TCP, TCP_KEEPCNT) == 2);
    CHECK(GetIntOpt(p.client.get(), IPPROTO_TCP, TCP_USER_TIMEOUT) == 20000);

    // 数据与关闭
    CHECK(::send(p.client.get(), "hello", 5, 0) == 5);
    CHECK(WaitFor(p.server.get(), POLLIN));
    char buf[8];
    CHECK(::recv(p.server.get(), buf, sizeof(buf), 0) == 5 && std::memcmp(buf, "hello", 5) == 0);
    p.client.reset();
    CHECK(WaitFor(p.server.get(), POLLIN));
    CHECK(::recv(p.server.get(), buf, sizeof(buf), 0) == 0); // 对端关闭
}

void TestTcpConnectRefused()
{
    // 先占一个端口再关掉，保证这个端口上没人监听。
    const auto any = Endpoint::Tcp("127.0.0.1", 0);
    std::optional<Endpoint> dead;
    {
        const UniqueFd l = Listener(*any);
        CHECK(sock::LocalEndpoint(l.get(), dead) == 0);
    }
    UniqueFd c;
    CHECK(sock::CreateStreamSocket(*dead, c) == 0);
    const int rc = sock::Connect(c.get(), *dead);
    if (rc == ECONNREFUSED)
        return; // 回环上有时直接同步失败
    CHECK(rc == EINPROGRESS);
    CHECK(WaitFor(c.get(), POLLOUT));
    CHECK(sock::GetSocketError(c.get()) == ECONNREFUSED);
    CHECK(sock::GetSocketError(c.get()) == 0); // 读一次就清除
}

void TestTcpBindInUse()
{
    const auto any = Endpoint::Tcp("127.0.0.1", 0);
    const UniqueFd l1 = Listener(*any);
    std::optional<Endpoint> local;
    CHECK(sock::LocalEndpoint(l1.get(), local) == 0);
    UniqueFd second;
    CHECK(sock::CreateStreamSocket(*local, second) == 0);
    CHECK(sock::Bind(second.get(), *local) == EADDRINUSE);
}

void TestTcpV6IfAvailable()
{
    const auto any = Endpoint::Tcp("::1", 0);
    CHECK(any.has_value());
    UniqueFd l;
    if (sock::CreateStreamSocket(*any, l) != 0)
        return; // 没有 IPv6 的环境跳过
    const int rc = sock::Bind(l.get(), *any);
    if (rc == EADDRNOTAVAIL || rc == EAFNOSUPPORT)
        return;
    CHECK(rc == 0 && sock::Listen(l.get(), 4) == 0);
    std::optional<Endpoint> local;
    CHECK(sock::LocalEndpoint(l.get(), local) == 0);
    CHECK(local.has_value() && local->IsIPv6() && local->address() == "::1");
    Pair p = ConnectAndAccept(l, *local);
    CHECK(p.peer.has_value() && p.peer->IsIPv6());
}

// 每个 UDS 用例用自己的临时目录，结束时清理。
struct TmpDir
{
    std::string dir;
    TmpDir()
    {
        char tmpl[] = "/tmp/breeze_sock_XXXXXX";
        CHECK(::mkdtemp(tmpl) != nullptr);
        dir = tmpl;
    }
    ~TmpDir()
    {
        for (const char* n : {"/a.sock", "/plain"})
            ::unlink((dir + n).c_str());
        ::rmdir(dir.c_str());
    }
    std::string Path(const char* name) const { return dir + "/" + name; }
};

void TestUdsRoundTripAndPeerCredentials()
{
    const TmpDir tmp;
    const auto ep = Endpoint::Unix(tmp.Path("a.sock"));
    CHECK(ep.has_value());
    const UniqueFd l = Listener(*ep);
    Pair p = ConnectAndAccept(l, *ep);

    CHECK(!p.peer.has_value()); // 客户端没有绑定路径：未命名，不是错误

    PeerCredentials cred;
    CHECK(uds::GetPeerCredentials(p.server.get(), cred) == 0);
    CHECK(cred.uid == ::getuid());
    CHECK(cred.gid == ::getgid());
    CHECK(cred.pid == ::getpid());

    CHECK(::send(p.client.get(), "ping", 4, 0) == 4);
    CHECK(WaitFor(p.server.get(), POLLIN));
    char buf[8];
    CHECK(::recv(p.server.get(), buf, sizeof(buf), 0) == 4 && std::memcmp(buf, "ping", 4) == 0);

    UniqueFd none;
    std::optional<Endpoint> peer;
    CHECK(sock::Accept(l.get(), none, peer) == EAGAIN);
}

// bind 前对残留的 socket 文件做探测：没人监听才清理，有人监听报 EADDRINUSE，不是 socket 的文件不动。
void TestUdsStaleSocketFile()
{
    const TmpDir tmp;
    const auto ep = Endpoint::Unix(tmp.Path("a.sock"));
    CHECK(ep.has_value());

    {
        const UniqueFd live = Listener(*ep);
        // 有人在监听：第二次 bind 必须失败，而且不能把对方的 socket 文件删掉
        UniqueFd second;
        CHECK(sock::CreateStreamSocket(*ep, second) == 0);
        CHECK(sock::Bind(second.get(), *ep) == EADDRINUSE);
        Pair p = ConnectAndAccept(live, *ep); // 原来的监听者仍然可用
        CHECK(p.server.valid());
    }

    // 监听者退出后文件还在（关闭 fd 不会 unlink），是残留
    struct stat st{};
    CHECK(::lstat(ep->address().c_str(), &st) == 0 && S_ISSOCK(st.st_mode));
    const UniqueFd fresh = Listener(*ep); // 探测到没人监听 → 清掉残留 → bind 成功
    Pair p = ConnectAndAccept(fresh, *ep);
    CHECK(p.server.valid());
}

void TestUdsRegularFileIsNotTouched()
{
    const TmpDir tmp;
    const std::string path = tmp.Path("plain");
    const int f = ::open(path.c_str(), O_CREAT | O_WRONLY, 0600);
    CHECK(f >= 0);
    ::close(f);

    const auto ep = Endpoint::Unix(path);
    CHECK(ep.has_value());
    UniqueFd fd;
    CHECK(sock::CreateStreamSocket(*ep, fd) == 0);
    CHECK(sock::Bind(fd.get(), *ep) == EADDRINUSE);
    struct stat st{};
    CHECK(::lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)); // 普通文件还在
}

void TestUdsConnectToMissingPath()
{
    const TmpDir tmp;
    const auto ep = Endpoint::Unix(tmp.Path("a.sock"));
    UniqueFd c;
    CHECK(sock::CreateStreamSocket(*ep, c) == 0);
    CHECK(sock::Connect(c.get(), *ep) == ENOENT); // sidecar 还没创建 socket 文件
}

void TestAbstract()
{
    const auto ep = Endpoint::UnixAbstract("breeze-test-" + std::to_string(::getpid()));
    CHECK(ep.has_value());
    const UniqueFd l = Listener(*ep);
    UniqueFd second;
    CHECK(sock::CreateStreamSocket(*ep, second) == 0);
    CHECK(sock::Bind(second.get(), *ep) == EADDRINUSE);

    Pair p = ConnectAndAccept(l, *ep);
    PeerCredentials cred;
    CHECK(uds::GetPeerCredentials(p.server.get(), cred) == 0 && cred.pid == ::getpid());
}

} // namespace

int main()
{
    TestTcpLoopback();
    TestTcpConnectRefused();
    TestTcpBindInUse();
    TestTcpV6IfAvailable();
    TestUdsRoundTripAndPeerCredentials();
    TestUdsStaleSocketFile();
    TestUdsRegularFileIsNotTouched();
    TestUdsConnectToMissingPath();
    TestAbstract();
    std::puts("socket_test: ok");
    return 0;
}
