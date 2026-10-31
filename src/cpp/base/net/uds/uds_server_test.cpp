// UdsServer / UdsClient：类型化的端点与选项、回显收发（路径与抽象名）、AcceptFilter 看到对端的 credentials、
// 残留 socket 文件的清理、选项类型里没有 TCP 专有的东西。
// 共用的 accept / 重连 / 背压逻辑由 stream_server_test、stream_client_test 覆盖，这里只测 UDS 这一层。
// 无测试框架依赖：失败即 abort。

#include "uds_server.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>

#include "event_loop_group.h"
#include "length_prefix_codec.h"
#include "net_test_util.h"
#include "socket.h"
#include "tcp_endpoint.h"
#include "uds_client.h"

using namespace breeze;
using namespace breeze::testing;

namespace
{

constexpr std::size_t kMaxFrame = 1 << 20;

// 基类的多态销毁；只收 UdsEndpoint：通用 Endpoint 与 TCP 的端点都传不进来。
static_assert(std::is_base_of_v<StreamServer, UdsServer> && std::is_base_of_v<StreamClient, UdsClient>);
template <typename Server, typename Ep, typename Opts>
inline constexpr bool kServerCtor =
    std::is_constructible_v<Server, EventLoopGroup&, const Ep&, Opts, ConnectionHandler&, const FrameCodecFactory&>;
template <typename Client, typename Ep, typename Opts>
inline constexpr bool kClientCtor = std::is_constructible_v<Client,
                                                            EventLoopGroup&,
                                                            const Ep&,
                                                            Opts,
                                                            ReconnectPolicy,
                                                            ConnectionHandler&,
                                                            const FrameCodecFactory&>;
static_assert(kServerCtor<UdsServer, UdsEndpoint, UdsServerOptions>);
static_assert(!kServerCtor<UdsServer, TcpEndpoint, UdsServerOptions> &&
              !kServerCtor<UdsServer, Endpoint, UdsServerOptions>);
static_assert(kClientCtor<UdsClient, UdsEndpoint, UdsClientOptions>);
static_assert(!kClientCtor<UdsClient, TcpEndpoint, UdsClientOptions> &&
              !kClientCtor<UdsClient, Endpoint, UdsClientOptions>);

// UDS 的选项里没有 TCP 专有的东西：以前配了会被静默忽略，现在连写都写不出来。
template <typename T>
concept HasTcpOptions = requires(T o) { o.tcp; };
template <typename T>
concept IsUdsServerOptions = requires(T o) {
    o.connection.high_water_mark;
    o.accept_filter;
    o.backlog;
};
template <typename T>
concept IsUdsClientOptions = requires(T o) {
    o.connection.idle_timeout;
    o.connect_timeout;
    o.on_connect_failed;
};
static_assert(!HasTcpOptions<UdsServerOptions> && !HasTcpOptions<UdsClientOptions>);
static_assert(IsUdsServerOptions<UdsServerOptions> && IsUdsClientOptions<UdsClientOptions>);

struct Fixture
{
    Fixture(const UdsEndpoint& ep, UdsServerOptions so = {})
        : factory(kMaxFrame)
        , group({2, 64})
        , server(group, ep, std::move(so), srv, factory)
    {
        srv.on_message = Echo;
        CHECK(group.Start() == 0);
        CHECK(server.Start() == 0);
    }
    EventRecorder srv;
    LengthPrefixCodecFactory factory;
    EventLoopGroup group;
    UdsServer server;
};

UdsEndpoint PathIn(const TempDir& dir, const char* name)
{
    const auto ep = UdsEndpoint::Path(dir.path() + "/" + name);
    CHECK(ep.has_value());
    return *ep;
}

void EchoOnce(const UdsEndpoint& ep)
{
    Fixture f(ep);
    CHECK(f.server.local_endpoint() == ep);

    EventRecorder cli;
    UdsClient client(f.group, ep, {}, ReconnectPolicy{}, cli, f.factory);
    CHECK(client.peer() == ep);
    CHECK(client.Start() == 0);
    CHECK(cli.WaitConnected(0).valid());
    CHECK(client.Send(Buf("ping")) == SendResult::Ok);
    CHECK(cli.WaitMessages(1));
    CHECK(cli.payloads()[0] == "ping");
    client.Stop();
    CHECK(cli.WaitClosed(0).reason == CloseReason::LocalClose);
    CHECK(WaitUntil([&] { return f.srv.closed_count() == 1; }));
    cli.CheckInvariants();
    f.srv.CheckInvariants();
}

void TestServerAndClientEchoOnPath()
{
    const LeakCheck leak;
    const TempDir dir;
    EchoOnce(PathIn(dir, "a.sock"));
}

void TestServerAndClientEchoOnAbstractName()
{
    const LeakCheck leak;
    const auto ep = UdsEndpoint::Abstract("breeze-uds-server-test-" + std::to_string(::getpid()));
    CHECK(ep.has_value());
    EchoOnce(*ep);
}

// UDS 的 PeerInfo 带对端进程的 credentials；客户端没有绑定路径，所以没有 endpoint。
void TestAcceptFilterSeesCredentials()
{
    const LeakCheck leak;
    const TempDir dir;
    std::atomic<int> match{0}; // 1 = credentials 与本进程一致，-1 = 不一致
    std::atomic<int> has_endpoint{-1};
    UdsServerOptions so;
    so.accept_filter = [&](const PeerInfo& p) noexcept
    {
        match = (p.credentials && p.credentials->pid == ::getpid() && p.credentials->uid == ::getuid() &&
                 p.credentials->gid == ::getgid())
                    ? 1
                    : -1;
        has_endpoint = p.endpoint.has_value() ? 1 : 0;
        return true;
    };
    Fixture f(PathIn(dir, "a.sock"), std::move(so));
    TestClient c(f.server.endpoint());
    c.SendFrame("hi");
    CHECK(c.RecvFrame() == "hi");
    CHECK(match == 1 && has_endpoint == 0);
}

void TestAcceptFilterRejectsByIdentity()
{
    const LeakCheck leak;
    const TempDir dir;
    UdsServerOptions so;
    so.accept_filter = [](const PeerInfo& p) noexcept { return p.credentials && p.credentials->uid != ::getuid(); };
    Fixture f(PathIn(dir, "a.sock"), std::move(so)); // 只放行别的用户：本进程自己会被拒
    TestClient c(f.server.endpoint());
    CHECK(c.WaitClosed());
    CHECK(f.group.GetStats().rejected_filter == 1);
}

// 通用的连接选项（idle_timeout）经类型化的选项传到连接上：服务端与客户端各一条。
void TestConnectionOptionsReachConnections()
{
    const LeakCheck leak;
    const TempDir dir;
    {
        UdsServerOptions so;
        so.connection.idle_timeout = 300ms;
        Fixture f(PathIn(dir, "a.sock"), std::move(so));
        TestClient idle(f.server.endpoint());
        CHECK(f.srv.WaitClosed(0).reason == CloseReason::IdleTimeout);
    }
    {
        Fixture f(PathIn(dir, "b.sock"));
        EventRecorder cli;
        UdsClientOptions co;
        co.connection.idle_timeout = 300ms;
        UdsClient client(f.group, f.server.local_endpoint(), std::move(co), ReconnectPolicy{}, cli, f.factory);
        CHECK(client.Start() == 0);
        CHECK(cli.WaitClosed(0).reason == CloseReason::IdleTimeout);
        client.Stop();
    }
}

// connect_timeout 经类型化的选项传到基类：≤ 0 是不合法选项，Start 返回 EINVAL。
void TestClientConnectTimeoutReachesBase()
{
    const LeakCheck leak;
    const TempDir dir;
    Fixture f(PathIn(dir, "a.sock"));
    EventRecorder cli;
    UdsClientOptions co;
    co.connect_timeout = 0ms;
    UdsClient client(f.group, f.server.local_endpoint(), std::move(co), ReconnectPolicy{}, cli, f.factory);
    CHECK(client.Start() == EINVAL);
}

// on_connect_failed 经类型化的选项传到基类：对端还没创建 socket 文件，得到 ENOENT 且会重试。
void TestConnectFailedCallbackReaches()
{
    const LeakCheck leak;
    const TempDir dir;
    Fixture f(PathIn(dir, "a.sock"));
    EventRecorder cli;
    std::atomic<int> err{0};
    std::atomic<int> retry{-1};
    UdsClientOptions co;
    co.on_connect_failed = [&](int e, bool will_retry) noexcept
    {
        retry = will_retry ? 1 : 0;
        err = e;
    };
    UdsClient client(f.group, PathIn(dir, "missing.sock"), std::move(co), ReconnectPolicy{}, cli, f.factory);
    CHECK(client.Start() == 0);
    CHECK(WaitUntil([&] { return err.load() != 0; }));
    CHECK(err == ENOENT && retry == 1);
    client.Stop();
}

// 残留的 socket 文件（上次进程没来得及清理）在 Start 时被清掉；不是 socket 的文件不动，Start 报 EADDRINUSE。
void TestStaleSocketFileAndRegularFile()
{
    const LeakCheck leak;
    const TempDir dir;
    const UdsEndpoint ep = PathIn(dir, "a.sock");
    {
        UniqueFd fd;
        CHECK(sock::CreateStreamSocket(ep.endpoint(), fd) == 0);
        CHECK(sock::Bind(fd.get(), ep.endpoint()) == 0); // 只 bind 不 listen，关掉后文件还在
    }
    struct stat st{};
    CHECK(::lstat(ep.address().c_str(), &st) == 0 && S_ISSOCK(st.st_mode));
    {
        Fixture f(ep); // Start 成功：残留被清掉
        EventRecorder cli;
        UdsClient client(f.group, ep, {}, ReconnectPolicy{}, cli, f.factory);
        CHECK(client.Start() == 0);
        CHECK(cli.WaitConnected(0).valid());
    }
    CHECK(::lstat(ep.address().c_str(), &st) != 0 && errno == ENOENT); // 停机后 socket 文件被删掉

    const UdsEndpoint plain = PathIn(dir, "plain");
    const int fd = ::open(plain.address().c_str(), O_CREAT | O_WRONLY, 0600);
    CHECK(fd >= 0);
    ::close(fd);
    EventRecorder rec;
    LengthPrefixCodecFactory factory(kMaxFrame);
    EventLoopGroup group({1, 8});
    CHECK(group.Start() == 0);
    UdsServer s(group, plain, {}, rec, factory);
    CHECK(s.Start() == EADDRINUSE);
    CHECK(::lstat(plain.address().c_str(), &st) == 0 && S_ISREG(st.st_mode)); // 文件原封不动
}

} // namespace

int main()
{
    TestServerAndClientEchoOnPath();
    TestServerAndClientEchoOnAbstractName();
    TestAcceptFilterSeesCredentials();
    TestAcceptFilterRejectsByIdentity();
    TestConnectionOptionsReachConnections();
    TestClientConnectTimeoutReachesBase();
    TestConnectFailedCallbackReaches();
    TestStaleSocketFileAndRegularFile();
    std::puts("uds_server_test: ok");
    return 0;
}
