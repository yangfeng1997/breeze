// StreamServer：Start 的错误与回滚、连接在 group 里的分布、AcceptFilter、过载拒绝、Stop 只停 accept、析构、
// 选项透传、统计，以及 TCP 与 UDS 两个 server 共用一个 group。
// 无测试框架依赖：失败即 abort。每个用例结束时检查没有 fd / 线程泄漏。

#include "stream_server.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "event_loop_group.h"
#include "group_access.h"
#include "length_prefix_codec.h"
#include "net_test_util.h"

using namespace breeze;
using namespace breeze::testing;

namespace
{

constexpr std::size_t kMaxFrame = 4 * 1024 * 1024;

// 回显 handler + group + 一个 server 的最小装配。
struct Fixture
{
    explicit Fixture(const Endpoint& ep,
                     EventLoopGroupOptions go = {2, 256},
                     StreamServerOptions so = {},
                     bool echo = true)
        : factory(kMaxFrame)
        , group(go)
        , server(group, ep, std::move(so), rec, factory)
    {
        if (echo)
            rec.on_message = Echo;
        CHECK(group.Start() == 0);
        CHECK(server.Start() == 0);
    }

    EventRecorder rec;
    LengthPrefixCodecFactory factory;
    EventLoopGroup group;
    StreamServer server;
};

Endpoint LoopbackAnyPort() { return *Endpoint::Tcp("127.0.0.1", 0); }

// 阻塞地试一次连接，返回 connect 的最终结果（0 = 连上了，否则 errno）。
int TryConnect(const Endpoint& ep)
{
    UniqueFd fd;
    CHECK(sock::CreateStreamSocket(ep, fd) == 0);
    int rc = sock::Connect(fd.get(), ep);
    if (rc == EINPROGRESS)
    {
        pollfd p{fd.get(), POLLOUT, 0};
        CHECK(::poll(&p, 1, 5000) > 0);
        rc = sock::GetSocketError(fd.get());
    }
    return rc;
}

// ---- Start 的错误与回滚 ----

void TestStartErrorsRollBack()
{
    const LeakCheck leak;
    TempDir dir;
    EventRecorder rec;
    LengthPrefixCodecFactory factory;
    EventLoopGroup group({2, 16});
    CHECK(group.Start() == 0);

    // 端口被占：EADDRINUSE
    StreamServer first(group, LoopbackAnyPort(), {}, rec, factory);
    CHECK(first.Start() == 0);
    CHECK(first.endpoint().port() != 0); // 端口填 0 时，成功后这里是系统分配的
    StreamServer clash(group, first.endpoint(), {}, rec, factory);
    CHECK(clash.Start() == EADDRINUSE);

    // UDS：有人在监听 → EADDRINUSE；路径上是普通文件 → EADDRINUSE 且文件不被动
    const Endpoint uds = dir.Unix("a.sock");
    StreamServer u1(group, uds, {}, rec, factory);
    CHECK(u1.Start() == 0);
    StreamServer u2(group, uds, {}, rec, factory);
    CHECK(u2.Start() == EADDRINUSE);
    const Endpoint file_ep = dir.Unix("plain");
    CHECK(::close(::open((dir.path() + "/plain").c_str(), O_CREAT | O_WRONLY, 0600)) == 0);
    StreamServer u3(group, file_ep, {}, rec, factory);
    CHECK(u3.Start() == EADDRINUSE);
    struct stat st{};
    CHECK(::stat((dir.path() + "/plain").c_str(), &st) == 0 && S_ISREG(st.st_mode));

    // 残留的 socket 文件（没人监听）：清掉后 bind 成功
    const Endpoint stale = dir.Unix("stale.sock");
    {
        UniqueFd fd;
        CHECK(sock::CreateStreamSocket(stale, fd) == 0);
        CHECK(sock::Bind(fd.get(), stale) == 0); // fd 关闭后文件仍在，没人监听
    }
    StreamServer u4(group, stale, {}, rec, factory);
    CHECK(u4.Start() == 0);

    // 失败的 server 不留 fd：能再用同一个对象之外的新对象在被占端口释放后 Start
    first.Stop();
    CHECK(WaitUntil([&] { return TryConnect(first.endpoint()) != 0; }));
    StreamServer again(group, first.endpoint(), {}, rec, factory);
    CHECK(again.Start() == 0);
}

// ---- TCP 与 UDS 两个 server 共用一个 group ----

void TestTcpAndUdsShareOneGroup()
{
    const LeakCheck leak;
    TempDir dir;
    EventRecorder rec;
    rec.on_message = Echo;
    LengthPrefixCodecFactory factory(kMaxFrame);
    EventLoopGroup group({2, 64});
    CHECK(group.Start() == 0);
    StreamServer tcp(group, LoopbackAnyPort(), {}, rec, factory);
    StreamServer uds(group, dir.Unix("g.sock"), {}, rec, factory);
    CHECK(tcp.Start() == 0);
    CHECK(uds.Start() == 0);

    TestClient a(tcp.endpoint());
    TestClient b(uds.endpoint());
    a.SendFrame("over-tcp");
    b.SendFrame("over-uds");
    CHECK(a.RecvFrame() == "over-tcp");
    CHECK(b.RecvFrame() == "over-uds");
    CHECK(rec.connected_count() == 2);

    a.Close();
    b.Close();
    CHECK(WaitUntil([&] { return rec.closed_count() == 2; }));
    rec.CheckInvariants();
}

// ---- 连接数最少的 loop 优先 ----

void TestConnectionsSpreadAcrossLoops()
{
    const LeakCheck leak;
    Fixture f(LoopbackAnyPort(), {4, 64});
    std::vector<std::unique_ptr<TestClient>> clients;
    for (int i = 0; i < 16; ++i)
        clients.push_back(std::make_unique<TestClient>(f.server.endpoint()));
    CHECK(f.rec.WaitConnected(15).valid());
    CHECK(WaitUntil([&] { return f.rec.connected_count() == 16; }));

    std::size_t lo = SIZE_MAX, hi = 0;
    for (std::size_t i = 0; i < GroupAccess::LoopCount(f.group); ++i)
    {
        const std::size_t n = GroupAccess::LoopAt(f.group, i).table().Live();
        lo = std::min(lo, n);
        hi = std::max(hi, n);
    }
    CHECK(hi - lo <= 1); // 预留名额在 accept 线程上先占，所以突发 accept 也是均匀的
    CHECK(f.group.GetStats().connections == 16);
}

// ---- AcceptFilter ----

void TestAcceptFilterRejectsWithoutCallbacks()
{
    const LeakCheck leak;
    std::atomic<bool> allow{false};
    std::atomic<int> calls{0};
    StreamServerOptions so;
    so.accept_filter = [&](const PeerInfo& p) noexcept
    {
        calls.fetch_add(1);
        CHECK(p.endpoint.has_value()); // TCP：对端地址
        return allow.load();
    };
    Fixture f(LoopbackAnyPort(), {2, 64}, std::move(so));

    TestClient rejected(f.server.endpoint());
    CHECK(rejected.WaitClosed()); // 被立即关闭
    CHECK(calls.load() == 1);
    CHECK(f.group.GetStats().rejected_filter == 1);
    std::this_thread::sleep_for(50ms);
    CHECK(f.rec.connected_count() == 0 && f.rec.closed_count() == 0); // 不产生任何回调

    allow = true;
    TestClient ok(f.server.endpoint());
    ok.SendFrame("hi");
    CHECK(ok.RecvFrame() == "hi");
    CHECK(f.group.GetStats().accepted == 1);
}

void TestAcceptFilterSeesUdsCredentials()
{
    const LeakCheck leak;
    TempDir dir;
    std::atomic<bool> seen{false};
    StreamServerOptions so;
    so.accept_filter = [&](const PeerInfo& p) noexcept
    {
        // UDS 客户端通常未命名（endpoint 为空），要识别对端看 credentials
        CHECK(p.credentials.has_value());
        CHECK(p.credentials->uid == ::getuid() && p.credentials->pid == ::getpid());
        seen = true;
        return true;
    };
    Fixture f(dir.Unix("f.sock"), {1, 8}, std::move(so));
    TestClient c(f.server.endpoint());
    c.SendFrame("x");
    CHECK(c.RecvFrame() == "x");
    CHECK(seen.load());
}

// ---- 过载：所有 loop 的连接表都满 ----

void TestTableFullRejectsAndRecovers()
{
    const LeakCheck leak;
    Fixture f(LoopbackAnyPort(), {1, 2});
    auto c1 = std::make_unique<TestClient>(f.server.endpoint());
    TestClient c2(f.server.endpoint());
    CHECK(WaitUntil([&] { return f.rec.connected_count() == 2; }));

    TestClient c3(f.server.endpoint());
    CHECK(c3.WaitClosed()); // 满了：accept 后立即关闭
    CHECK(f.group.GetStats().rejected_full == 1);
    std::this_thread::sleep_for(50ms);
    CHECK(f.rec.connected_count() == 2 && f.rec.closed_count() == 0); // 被拒的连接没有任何回调

    c1.reset(); // 腾出一个名额
    CHECK(WaitUntil([&] { return f.rec.closed_count() == 1; }));
    TestClient c4(f.server.endpoint());
    c4.SendFrame("again");
    CHECK(c4.RecvFrame() == "again");
    f.rec.CheckInvariants();
}

// ---- Stop 只停 accept ----

void TestStopOnlyStopsAccept()
{
    const LeakCheck leak;
    Fixture f(LoopbackAnyPort());
    TestClient c(f.server.endpoint());
    const ConnectionHandle h = f.rec.WaitConnected(0);
    c.SendFrame("before");
    CHECK(c.RecvFrame() == "before");

    f.server.Stop();
    // Stop 是异步的：生效之前探测用的连接也会被接受、随后被关掉，所以下面只断言老连接，不数总数。
    CHECK(WaitUntil([&] { return TryConnect(f.server.endpoint()) != 0; })); // 新连接进不来
    c.SendFrame("after");                                                   // 老连接照常服务
    CHECK(c.RecvFrame() == "after");
    CHECK(!f.rec.WasClosed(h));
    f.server.Stop(); // 重复调用是空操作
}

void TestStopBeforeStartIsNoop()
{
    const LeakCheck leak;
    EventRecorder rec;
    LengthPrefixCodecFactory factory;
    EventLoopGroup group({1, 8});
    CHECK(group.Start() == 0);
    StreamServer s(group, LoopbackAnyPort(), {}, rec, factory);
    s.Stop();
}

// 析构时 group 仍在运行：同步停掉 accept，端口随之释放。
void TestDestructorReleasesPort()
{
    const LeakCheck leak;
    EventRecorder rec;
    LengthPrefixCodecFactory factory;
    EventLoopGroup group({2, 8});
    CHECK(group.Start() == 0);
    Endpoint ep = LoopbackAnyPort();
    {
        StreamServer s(group, ep, {}, rec, factory);
        CHECK(s.Start() == 0);
        ep = s.endpoint();
        CHECK(TryConnect(ep) == 0);
    }
    CHECK(TryConnect(ep) != 0);
    StreamServer again(group, ep, {}, rec, factory);
    CHECK(again.Start() == 0);
}

// ---- 选项透传 ----

void TestOptionsReachConnections()
{
    const LeakCheck leak;

    // 水位：客户端不读，经句柄灌数据 → SlowConsumer
    {
        StreamServerOptions so;
        so.connection.high_water_mark = 64 * 1024;
        Fixture f(LoopbackAnyPort(), {1, 8}, std::move(so), /*echo=*/false);
        TestClient c(f.server.endpoint());
        const ConnectionHandle h = f.rec.WaitConnected(0);
        bool closed = false;
        for (int i = 0; i < 400 && !closed; ++i) // 内核缓冲吸收几 MB 之后积压才会涨
            closed = h.Send(SharedBuffer::Create(256 * 1024)) == SendResult::Closed;
        CHECK(closed);
        CHECK(f.rec.WaitClosed(0).reason == CloseReason::SlowConsumer);
        CHECK(f.group.GetStats().send_closed_slow_consumer >= 1);
    }

    // idle：什么都不发 → IdleTimeout（精度 ≤ 1s，加上 300ms 的超时）
    {
        StreamServerOptions so;
        so.connection.idle_timeout = 300ms;
        Fixture f(LoopbackAnyPort(), {1, 8}, std::move(so), /*echo=*/false);
        TestClient idle(f.server.endpoint());
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(f.rec.WaitClosed(0).reason == CloseReason::IdleTimeout);
        CHECK(std::chrono::steady_clock::now() - t0 >= 250ms);
        CHECK(idle.WaitClosed());
    }
}

// ---- 统计 ----

// 接口：StreamServer 是 NetServer；NetModule 与业务经 NetServer 统一持有各种服务端。
static_assert(std::is_base_of_v<NetServer, StreamServer>);

// UDP 是数据报端点，字节流的服务端不收它：Start 返回 EPROTONOSUPPORT，不会悄悄建一个 TCP 监听
// （地址写法与 TCP 相同，不拦住的话 "udp://1.2.3.4:80" 会变成 1.2.3.4:80 的 TCP 监听）。
// 这个检查先于 group 的状态检查，group 没起来也不是编程错误。
void TestDatagramEndpointIsRejected()
{
    const LeakCheck leak;
    EventRecorder rec;
    LengthPrefixCodecFactory factory(kMaxFrame);
    EventLoopGroup group({1, 8});
    StreamServer s(group, *Endpoint::Udp("127.0.0.1", 0), {}, rec, factory);
    CHECK(s.Start() == EPROTONOSUPPORT); // group 还没 Start
    CHECK(group.Start() == 0);
    CHECK(s.Start() == EPROTONOSUPPORT);
    CHECK(s.endpoint().kind() == Endpoint::Kind::Udp);
    CHECK(group.GetStats().connections == 0 && group.GetStats().accepted == 0);
    s.Stop(); // 没起来过：空操作
}

void TestStatsAddUp()
{
    const LeakCheck leak;
    Fixture f(LoopbackAnyPort(), {2, 64});
    constexpr int kClients = 6;
    constexpr int kMsgs = 20;
    std::vector<std::unique_ptr<TestClient>> cs;
    for (int i = 0; i < kClients; ++i)
        cs.push_back(std::make_unique<TestClient>(f.server.endpoint()));
    for (auto& c : cs)
    {
        for (int m = 0; m < kMsgs; ++m)
        {
            c->SendFrame("0123456789"); // 10 字节 payload + 4 字节帧头
            CHECK(c->RecvFrame() == "0123456789");
        }
    }
    // 客户端收到最后一条回包时，IO 线程可能还没把计数记上：messages_in 在 OnMessage 返回之后才记，messages_out 与
    // bytes_out 在 write 返回之后才记（NetStats 各计数本来就不是同一时刻的快照，见 net_stats.h）。先等到位再取快照。
    // connections / accepted / bytes_in 不用等：前两个在这条连接的第一个读事件之前就记了，
    // bytes_in 在 read 之后、分发之前记。
    CHECK(WaitUntil(
        [&]
        {
            const NetStats s = f.group.GetStats();
            return s.messages_in == kClients * kMsgs && s.messages_out == kClients * kMsgs &&
                   s.bytes_out == kClients * kMsgs * 14u;
        }));
    const NetStats mid = f.group.GetStats();
    CHECK(mid.connections == kClients && mid.accepted == kClients);
    CHECK(mid.messages_in == kClients * kMsgs && mid.messages_out == kClients * kMsgs);
    CHECK(mid.bytes_in == kClients * kMsgs * 14u && mid.bytes_out == kClients * kMsgs * 14u);
    CHECK(mid.max_loop_lag.count() > 0);

    cs.clear();
    CHECK(WaitUntil([&] { return f.rec.closed_count() == kClients; }));
    const NetStats end = f.group.GetStats();
    CHECK(end.closed[static_cast<std::size_t>(CloseReason::PeerClosed)] == kClients);
    CHECK(WaitUntil([&] { return f.group.GetStats().connections == 0; }));
    f.rec.CheckInvariants();
}

// ---- 冒烟：几千条连接分布在多个 loop 上 ----

void TestManyConnections()
{
    const LeakCheck leak;
    constexpr int kConns = 3000;
    Fixture f(LoopbackAnyPort(), {4, 2048});
    std::vector<std::unique_ptr<TestClient>> cs;
    cs.reserve(kConns);
    for (int i = 0; i < kConns; ++i)
        cs.push_back(std::make_unique<TestClient>(f.server.endpoint()));
    for (int i = 0; i < kConns; i += 7)
    {
        cs[static_cast<std::size_t>(i)]->SendFrame("ping" + std::to_string(i));
        CHECK(cs[static_cast<std::size_t>(i)]->RecvFrame() == "ping" + std::to_string(i));
    }
    CHECK(WaitUntil([&] { return f.rec.connected_count() == kConns; }));
    cs.clear();
    CHECK(WaitUntil([&] { return f.rec.closed_count() == kConns; }));
    f.rec.CheckInvariants();
}

} // namespace

int main()
{
    // 冒烟用例两端各要几千个 fd
    rlimit rl{};
    CHECK(::getrlimit(RLIMIT_NOFILE, &rl) == 0);
    rl.rlim_cur = std::min<rlim_t>(rl.rlim_max, 65536);
    CHECK(::setrlimit(RLIMIT_NOFILE, &rl) == 0);

    TestStartErrorsRollBack();
    TestTcpAndUdsShareOneGroup();
    TestConnectionsSpreadAcrossLoops();
    TestAcceptFilterRejectsWithoutCallbacks();
    TestAcceptFilterSeesUdsCredentials();
    TestTableFullRejectsAndRecovers();
    TestStopOnlyStopsAccept();
    TestStopBeforeStartIsNoop();
    TestDestructorReleasesPort();
    TestOptionsReachConnections();
    TestDatagramEndpointIsRejected();
    TestStatsAddUp();
    TestManyConnections();
    std::puts("stream_server_test: ok");
    return 0;
}
