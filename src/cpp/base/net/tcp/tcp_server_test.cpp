// TcpServer / TcpClient：类型化的端点与选项、回显收发、基类里的 accept 过滤对 TCP 看到的是 IP 而不是 credentials。
// 共用的 accept / 重连 / 背压逻辑由 stream_server_test、stream_client_test 覆盖，这里只测 TCP 这一层。
// 无测试框架依赖：失败即 abort。

#include "base/net/tcp/tcp_server.h"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <type_traits>

#include "base/net/codec/length_prefix_codec.h"
#include "base/net/loop/event_loop_group.h"
#include "base/net/stream/internal/stream_socket.h"
#include "base/net/tcp/tcp_client.h"
#include "base/net/testing/net_test_util.h"
#include "base/net/uds/uds_endpoint.h"

using namespace breeze;
using namespace breeze::testing;

namespace
{

constexpr std::size_t kMaxFrame = 1 << 20;

// 基类的多态销毁；只收 TcpEndpoint：通用 Endpoint 与 UDS 的端点都传不进来。
static_assert(std::is_base_of_v<StreamServer, TcpServer> && std::is_base_of_v<StreamClient, TcpClient>);
static_assert(std::has_virtual_destructor_v<StreamServer> && std::has_virtual_destructor_v<StreamClient>);
template <typename Server, typename Ep, typename Opts>
inline constexpr bool kServerCtor =
    std::is_constructible_v<Server, EventLoopGroup&, const Ep&, Opts, IConnectionHandler&, const IFrameCodecFactory&>;
template <typename Client, typename Ep, typename Opts>
inline constexpr bool kClientCtor = std::is_constructible_v<Client,
                                                            EventLoopGroup&,
                                                            const Ep&,
                                                            Opts,
                                                            ReconnectPolicy,
                                                            IConnectionHandler&,
                                                            const IFrameCodecFactory&>;
static_assert(kServerCtor<TcpServer, TcpEndpoint, TcpServerOptions>);
static_assert(!kServerCtor<TcpServer, UdsEndpoint, TcpServerOptions> &&
              !kServerCtor<TcpServer, Endpoint, TcpServerOptions>);
static_assert(kClientCtor<TcpClient, TcpEndpoint, TcpClientOptions>);
static_assert(!kClientCtor<TcpClient, UdsEndpoint, TcpClientOptions> &&
              !kClientCtor<TcpClient, Endpoint, TcpClientOptions>);

// TCP 专有的选项在 TcpOptions 里，不在每种传输都用的 ConnectionOptions 里。
template <typename T>
concept HasTcpOptions = requires(T o) {
    o.tcp.keepalive_idle;
    o.tcp.tcp_user_timeout;
};
template <typename T>
concept HasTcpFieldsDirectly = requires(T o) { o.keepalive_idle; } || requires(T o) { o.tcp_user_timeout; };
static_assert(HasTcpOptions<TcpServerOptions> && HasTcpOptions<TcpClientOptions>);
static_assert(!HasTcpFieldsDirectly<ConnectionOptions>);

struct Fixture
{
    explicit Fixture(TcpServerOptions so = {})
        : factory(kMaxFrame)
        , group({2, 64})
        , server(group, *TcpEndpoint::Make("127.0.0.1", 0), std::move(so), srv, factory)
    {
        srv.on_message = Echo;
        CHECK(group.Start() == 0);
        CHECK(server.Start() == 0);
    }
    EventRecorder srv;
    LengthPrefixCodecFactory factory;
    EventLoopGroup group;
    TcpServer server;
};

void TestServerAndClientEcho()
{
    const LeakCheck leak;
    Fixture f;
    const TcpEndpoint bound = f.server.local_endpoint();
    CHECK(bound.ip() == "127.0.0.1" && bound.port() != 0); // 端口 0 → 系统分配
    CHECK(f.server.endpoint() == bound.endpoint());        // 基类的通用形式一致

    EventRecorder cli;
    TcpClient client(f.group, bound, {}, ReconnectPolicy{}, cli, f.factory);
    CHECK(client.peer() == bound);
    CHECK(client.Start() == 0);
    const ConnectionHandle h = cli.WaitConnected(0);
    CHECK(h.valid() && client.handle() == h);

    CHECK(client.Send(Buf("hello")) == SendResult::Ok);
    CHECK(cli.WaitMessages(1));
    CHECK(cli.payloads()[0] == "hello");

    client.Stop();
    CHECK(cli.WaitClosed(0).reason == CloseReason::LocalClose);
    CHECK(WaitUntil([&] { return f.srv.closed_count() == 1; }));
    cli.CheckInvariants();
    f.srv.CheckInvariants();
}

// TCP 的 PeerInfo 只有对端的 IP 与端口，没有 credentials（那是 UDS 才有的）。
void TestAcceptFilterSeesIpNotCredentials()
{
    const LeakCheck leak;
    std::atomic<int> loopback_peer{0}; // 1 = 对端是 127.0.0.1，-1 = 不是
    std::atomic<int> credentials{-1};  // 1 = 带了 credentials
    TcpServerOptions so;
    so.accept_filter = [&](const PeerInfo& p) noexcept
    {
        loopback_peer = (p.endpoint && p.endpoint->address() == "127.0.0.1") ? 1 : -1;
        credentials = p.credentials.has_value() ? 1 : 0;
        return true;
    };
    Fixture f(std::move(so));
    TestClient c(f.server.endpoint());
    c.SendFrame("hi");
    CHECK(c.RecvFrame() == "hi");
    CHECK(loopback_peer == 1 && credentials == 0);
}

void TestAcceptFilterRejects()
{
    const LeakCheck leak;
    TcpServerOptions so;
    so.accept_filter = [](const PeerInfo&) noexcept { return false; };
    Fixture f(std::move(so));
    TestClient c(f.server.endpoint());
    CHECK(c.WaitClosed());
    CHECK(f.group.GetStats().rejected_filter == 1);
    CHECK(f.srv.connected_count() == 0);
}

// 选项到 socket 的端到端：用特殊值，在本进程的 fd 里找出设了它的 TCP socket（别处不会碰巧相同）。
int GetIntOpt(int fd, int level, int name)
{
    int v = -1;
    socklen_t len = sizeof(v);
    CHECK(::getsockopt(fd, level, name, &v, &len) == 0);
    return v;
}

int FindTcpSocketWithKeepIdle(int idle_s)
{
    for (int fd = 0; fd < 1024; ++fd)
    {
        int v = 0;
        socklen_t len = sizeof(v);
        if (::getsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &v, &len) == 0 && v == idle_s)
            return fd;
    }
    return -1;
}

void CheckKeepAliveOptions(int fd, int idle_s, int interval_s, int count, int user_timeout_ms)
{
    CHECK(GetIntOpt(fd, SOL_SOCKET, SO_KEEPALIVE) == 1);
    CHECK(GetIntOpt(fd, IPPROTO_TCP, TCP_KEEPIDLE) == idle_s);
    CHECK(GetIntOpt(fd, IPPROTO_TCP, TCP_KEEPINTVL) == interval_s);
    CHECK(GetIntOpt(fd, IPPROTO_TCP, TCP_KEEPCNT) == count);
    CHECK(GetIntOpt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT) == user_timeout_ms);
    CHECK(GetIntOpt(fd, IPPROTO_TCP, TCP_NODELAY) == 1);
}

void TestServerTcpOptionsReachAcceptedSocket()
{
    const LeakCheck leak;
    TcpServerOptions so;
    so.tcp.keepalive_idle = 37s;
    so.tcp.keepalive_interval = 7s;
    so.tcp.keepalive_count = 5;
    so.tcp.tcp_user_timeout = 23456ms;
    Fixture f(std::move(so));
    TestClient c(f.server.endpoint());
    f.srv.WaitConnected(0); // OnConnected 在 ConfigureSocket 之后
    const int fd = FindTcpSocketWithKeepIdle(37);
    CHECK(fd >= 0);
    CheckKeepAliveOptions(fd, 37, 7, 5, 23456);
}

void TestClientTcpOptionsReachConnectedSocket()
{
    const LeakCheck leak;
    Fixture f; // 服务端用默认选项（60s），所以 41s 的只能是客户端那条连接
    EventRecorder cli;
    TcpClientOptions co;
    co.tcp.keepalive_idle = 41s;
    co.tcp.keepalive_interval = 9s;
    co.tcp.keepalive_count = 4;
    co.tcp.tcp_user_timeout = 31000ms;
    TcpClient client(f.group, f.server.local_endpoint(), std::move(co), ReconnectPolicy{}, cli, f.factory);
    CHECK(client.Start() == 0);
    cli.WaitConnected(0);
    const int fd = FindTcpSocketWithKeepIdle(41);
    CHECK(fd >= 0);
    CheckKeepAliveOptions(fd, 41, 9, 4, 31000);
}

// 通用的连接选项（idle_timeout）经类型化的选项传到连接上：服务端与客户端各一条。
void TestConnectionOptionsReachConnections()
{
    const LeakCheck leak;
    {
        TcpServerOptions so;
        so.connection.idle_timeout = 300ms;
        Fixture f(std::move(so));
        TestClient idle(f.server.endpoint());
        CHECK(f.srv.WaitClosed(0).reason == CloseReason::IdleTimeout);
    }
    {
        Fixture f;
        EventRecorder cli;
        TcpClientOptions co;
        co.connection.idle_timeout = 300ms;
        TcpClient client(f.group, f.server.local_endpoint(), std::move(co), ReconnectPolicy{}, cli, f.factory);
        CHECK(client.Start() == 0);
        CHECK(cli.WaitClosed(0).reason == CloseReason::IdleTimeout);
        client.Stop();
    }
}

// connect_timeout 经类型化的选项传到基类：≤ 0 是不合法选项，Start 返回 EINVAL。
void TestClientConnectTimeoutReachesBase()
{
    const LeakCheck leak;
    Fixture f;
    EventRecorder cli;
    TcpClientOptions co;
    co.connect_timeout = 0ms;
    TcpClient client(f.group, f.server.local_endpoint(), std::move(co), ReconnectPolicy{}, cli, f.factory);
    CHECK(client.Start() == EINVAL);
}

// on_connect_failed 经类型化的选项传到基类：连一个没人监听的端口（先占一个再释放），得到 ECONNREFUSED 且会重试。
void TestConnectFailedCallbackReaches()
{
    const LeakCheck leak;
    std::optional<Endpoint> closed;
    {
        UniqueFd fd;
        CHECK(sock::CreateStreamSocket(*Endpoint::Tcp("127.0.0.1", 0), fd) == 0);
        CHECK(sock::Bind(fd.get(), *Endpoint::Tcp("127.0.0.1", 0)) == 0);
        CHECK(sock::LocalEndpoint(fd.get(), closed) == 0 && closed.has_value());
    } // 关掉：这个端口现在没人监听
    Fixture f;
    EventRecorder cli;
    std::atomic<int> err{0};
    std::atomic<int> retry{-1};
    TcpClientOptions co;
    co.on_connect_failed = [&](int e, bool will_retry) noexcept
    {
        retry = will_retry ? 1 : 0;
        err = e;
    };
    TcpClient client(f.group, *TcpEndpoint::From(*closed), std::move(co), ReconnectPolicy{}, cli, f.factory);
    CHECK(client.Start() == 0);
    CHECK(WaitUntil([&] { return err.load() != 0; }));
    CHECK(err == ECONNREFUSED && retry == 1);
    client.Stop();
}

void TestBindInUse()
{
    const LeakCheck leak;
    Fixture f;
    TcpServer second(f.group, f.server.local_endpoint(), {}, f.srv, f.factory);
    CHECK(second.Start() == EADDRINUSE);
}

} // namespace

int main()
{
    TestServerAndClientEcho();
    TestAcceptFilterSeesIpNotCredentials();
    TestAcceptFilterRejects();
    TestServerTcpOptionsReachAcceptedSocket();
    TestClientTcpOptionsReachConnectedSocket();
    TestConnectionOptionsReachConnections();
    TestClientConnectTimeoutReachesBase();
    TestConnectFailedCallbackReaches();
    TestBindInUse();
    std::puts("tcp_server_test: ok");
    return 0;
}
