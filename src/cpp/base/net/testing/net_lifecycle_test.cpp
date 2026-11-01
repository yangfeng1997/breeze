// EventLoopGroup::Shutdown(deadline) 的收尾：优雅排空、deadline 强关、与 accept 并发、Shutdown 之后不再有回调、
// 析构兜底不排空，以及 server 与 client 共用一个 group。
// 无测试框架依赖：失败即 abort。每个用例结束时检查没有 fd / 线程泄漏（含 Shutdown 之后 group 析构）。

#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "base/net/codec/length_prefix_codec.h"
#include "base/net/loop/event_loop_group.h"
#include "base/net/loop/group_access.h"
#include "base/net/stream/stream_client.h"
#include "base/net/stream/stream_server.h"
#include "base/net/testing/net_test_util.h"

using namespace breeze;
using namespace breeze::testing;

namespace
{

constexpr std::size_t kMaxFrame = 8 * 1024 * 1024;

StreamServerOptions BigWater()
{
    StreamServerOptions so;
    so.connection.high_water_mark = 256ull << 20;
    return so;
}

struct Fixture
{
    explicit Fixture(const Endpoint& ep, std::uint32_t loops = 2, StreamServerOptions so = BigWater())
        : factory(kMaxFrame)
        , group({loops, 256})
        , server(group, ep, std::move(so), rec, factory)
    {
        CHECK(group.Start() == 0);
        CHECK(server.Start() == 0);
    }
    EventRecorder rec;
    LengthPrefixCodecFactory factory;
    EventLoopGroup group;
    StreamServer server;
};

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

// 每条连接都回调过 OnClosed：停机之后 OnConnected 与 OnClosed 一一对应。
void CheckAllClosed(const EventRecorder& rec)
{
    CHECK(rec.connected_count() == rec.closed_count());
    rec.CheckInvariants();
}

// ---- 优雅排空 ----

// 积压的数据先发完，对端读到全部数据与 EOF、关闭之后连接正常结束；Shutdown 在 deadline 之前返回，返回时 OnClosed
// 已全部回调。
void TestShutdownDrainsGracefully(const Endpoint& ep)
{
    const LeakCheck leak;
    Fixture f(ep);
    constexpr int kClients = 6;
    constexpr std::size_t kBig = 1024 * 1024;
    std::vector<std::unique_ptr<TestClient>> clients;
    for (int i = 0; i < kClients; ++i)
        clients.push_back(std::make_unique<TestClient>(f.server.endpoint()));
    for (int i = 0; i < kClients; ++i)
        f.rec.WaitConnected(static_cast<std::size_t>(i));
    for (int i = 0; i < kClients; ++i)
    {
        SharedBuffer b = SharedBuffer::Create(kBig);
        std::memset(b.MutableBytes().data(), 'a' + i, kBig);
        CHECK(f.rec.WaitConnected(static_cast<std::size_t>(i)).Send(std::move(b)) == SendResult::Ok);
    }

    // 每个客户端：读完整条数据，等 EOF，再关。连接的回调顺序与客户端的创建顺序不一定一致（分在不同 loop 上），
    // 所以不假定哪个客户端收到哪个值，只要求每个客户端收到一整条内容一致的帧、六个值各出现一次。
    std::vector<std::thread> readers;
    std::mutex mu;
    std::string fronts;
    for (int i = 0; i < kClients; ++i)
    {
        readers.emplace_back(
            [&, i]
            {
                TestClient& c = *clients[static_cast<std::size_t>(i)];
                const std::string got = c.RecvFrame();
                const bool uniform = got.size() == kBig && got.find_first_not_of(got.front()) == std::string::npos;
                {
                    const std::lock_guard lk(mu);
                    fronts.push_back(uniform ? got.front() : '?');
                }
                CHECK(c.WaitClosed());
                c.Close();
            });
    }

    const auto t0 = std::chrono::steady_clock::now();
    f.group.Shutdown(5s);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    for (auto& t : readers)
        t.join();

    CHECK(elapsed < 4s); // 对端都关了，没有等到 deadline
    std::sort(fronts.begin(), fronts.end());
    CHECK(fronts == "abcdef"); // 积压的数据一条不丢、不串
    CheckAllClosed(f.rec);
    CHECK(f.rec.closed_count() == kClients);
    for (std::size_t i = 0; i < kClients; ++i)
        CHECK(f.rec.WaitClosed(i).reason == CloseReason::LocalClose);
    CHECK(!f.group.running());
    CHECK(f.rec.WaitConnected(0).Send(Buf("x")) == SendResult::Closed); // group 注销之后句柄解析不到
    CHECK(TryConnect(f.server.endpoint()) != 0);                        // accept 已经停了
}

// 对端不读、不关：到 deadline 强关，Shutdown 在 deadline 附近返回，OnClosed 照样全部回调。
void TestShutdownDeadlineForcesStuckConnections()
{
    const LeakCheck leak;
    Fixture f(*Endpoint::Tcp("127.0.0.1", 0));
    constexpr int kClients = 4;
    std::vector<std::unique_ptr<TestClient>> clients; // 不读
    for (int i = 0; i < kClients; ++i)
    {
        clients.push_back(std::make_unique<TestClient>(f.server.endpoint()));
        CHECK(f.rec.WaitConnected(static_cast<std::size_t>(i)).Send(SharedBuffer::Create(4 * 1024 * 1024)) ==
              SendResult::Ok);
    }
    const auto t0 = std::chrono::steady_clock::now();
    f.group.Shutdown(300ms);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    CHECK(elapsed >= 250ms && elapsed < 3s);
    CheckAllClosed(f.rec);
    CHECK(f.rec.closed_count() == kClients);
}

void TestShutdownZeroDeadlineClosesImmediately()
{
    const LeakCheck leak;
    Fixture f(*Endpoint::Tcp("127.0.0.1", 0));
    std::vector<std::unique_ptr<TestClient>> clients;
    for (int i = 0; i < 3; ++i)
        clients.push_back(std::make_unique<TestClient>(f.server.endpoint()));
    for (int i = 0; i < 3; ++i)
        f.rec.WaitConnected(static_cast<std::size_t>(i));
    const auto t0 = std::chrono::steady_clock::now();
    f.group.Shutdown(0ms);
    CHECK(std::chrono::steady_clock::now() - t0 < 2s);
    CheckAllClosed(f.rec);
    CHECK(f.rec.closed_count() == 3);
    for (auto& c : clients)
        CHECK(c->WaitClosed()); // 对端都得到了 EOF
}

// Shutdown 之后不再有任何回调；统计仍可读；重复 Shutdown 是空操作。
void TestNoCallbacksAfterShutdown()
{
    const LeakCheck leak;
    Fixture f(*Endpoint::Tcp("127.0.0.1", 0));
    f.rec.on_message = Echo;
    TestClient c(f.server.endpoint());
    c.SendFrame("x");
    CHECK(c.RecvFrame() == "x");
    f.group.Shutdown(1s);
    const std::size_t events = f.rec.connected_count() + f.rec.message_count() + f.rec.closed_count();
    f.group.Shutdown(1s);
    c.Close();
    std::this_thread::sleep_for(150ms);
    CHECK(f.rec.connected_count() + f.rec.message_count() + f.rec.closed_count() == events);
    const NetStats s = f.group.GetStats();
    CHECK(s.accepted == 1 && s.messages_in == 1 && s.connections == 0);
    CHECK(s.closed[static_cast<std::size_t>(CloseReason::LocalClose)] == 1);
}

// UDS：停机时 Acceptor 被停掉，socket 文件随之清掉。
void TestShutdownUnlinksUdsPath()
{
    const LeakCheck leak;
    TempDir dir;
    const Endpoint ep = dir.Unix("s.sock");
    Fixture f(ep);
    struct stat st{};
    CHECK(::stat((dir.path() + "/s.sock").c_str(), &st) == 0);
    f.group.Shutdown(500ms);
    CHECK(::stat((dir.path() + "/s.sock").c_str(), &st) != 0);
}

// 析构只做兜底：没 Shutdown 过就直接停，不排空，也就没有 OnClosed（那时 handler 可能已经销毁）。
void TestDestructorDoesNotDrain()
{
    const LeakCheck leak;
    EventRecorder rec;
    LengthPrefixCodecFactory factory(kMaxFrame);
    std::unique_ptr<TestClient> c;
    {
        EventLoopGroup group({2, 16});
        StreamServer server(group, *Endpoint::Tcp("127.0.0.1", 0), {}, rec, factory);
        CHECK(group.Start() == 0);
        CHECK(server.Start() == 0);
        c = std::make_unique<TestClient>(server.endpoint());
        rec.WaitConnected(0);
    }
    CHECK(rec.connected_count() == 1 && rec.closed_count() == 0);
    CHECK(c->WaitClosed()); // 连接随 loop 销毁而关闭，对端看到的是 EOF / RST
}

// ---- 与 accept 并发 ----

// 客户端线程不停地连、发、关，期间 Shutdown：不崩、不泄漏，建立过的连接都恰好回调一次 OnClosed。
void TestShutdownRacesWithAccepts()
{
    const LeakCheck leak;
    Fixture f(*Endpoint::Tcp("127.0.0.1", 0), 4, StreamServerOptions{});
    f.rec.on_message = Echo;
    std::atomic<bool> stop{false};
    std::atomic<int> attempts{0};
    const Endpoint ep = f.server.endpoint();
    std::vector<std::thread> clients;
    for (int t = 0; t < 4; ++t)
    {
        clients.emplace_back(
            [&]
            {
                while (!stop.load())
                {
                    UniqueFd fd;
                    if (sock::CreateStreamSocket(ep, fd) != 0)
                        continue;
                    const int rc = sock::Connect(fd.get(), ep);
                    if (rc != 0 && rc != EINPROGRESS)
                        continue;
                    pollfd p{fd.get(), POLLOUT, 0};
                    if (::poll(&p, 1, 200) <= 0 || sock::GetSocketError(fd.get()) != 0)
                        continue;
                    attempts.fetch_add(1);
                    const auto frame = TestClient::Frame("ping");
                    (void)::send(fd.get(), frame.data(), frame.size(), MSG_NOSIGNAL);
                    // 随 fd 析构关闭，且用 RST：本机临时端口只有几千个，正常 FIN 会留下 TIME_WAIT，攒多了拖累后面的用例
                    const linger lg{1, 0};
                    (void)::setsockopt(fd.get(), SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
                    std::this_thread::sleep_for(200us);
                }
            });
    }
    CHECK(WaitUntil([&] { return f.rec.connected_count() >= 50; }));
    f.group.Shutdown(500ms);
    stop = true;
    for (auto& t : clients)
        t.join();
    CHECK(attempts.load() >= 50);
    CheckAllClosed(f.rec);
}

// ---- server 与 client 共用一个 group ----

void TestServerAndClientInOneGroupShutDownTogether()
{
    const LeakCheck leak;
    EventRecorder srv_rec;
    srv_rec.on_message = Echo;
    EventRecorder cli_rec;
    LengthPrefixCodecFactory factory(kMaxFrame);
    EventLoopGroup group({2, 64});
    StreamServer server(group, *Endpoint::Tcp("127.0.0.1", 0), BigWater(), srv_rec, factory);
    CHECK(group.Start() == 0);
    CHECK(server.Start() == 0);
    StreamClient client(group, server.endpoint(), {}, ReconnectPolicy{20ms, 100ms, 2.0, 0.0}, cli_rec, factory);
    CHECK(client.Start() == 0);
    cli_rec.WaitConnected(0);
    for (int i = 0; i < 50; ++i)
        CHECK(client.Send(Buf("m" + std::to_string(i))) == SendResult::Ok);
    CHECK(cli_rec.WaitMessages(50));

    group.Shutdown(2s);
    CheckAllClosed(srv_rec);
    CheckAllClosed(cli_rec);
    CHECK(srv_rec.closed_count() == 1 && cli_rec.closed_count() == 1);
    const std::size_t n = srv_rec.connected_count();
    std::this_thread::sleep_for(300ms); // 客户端没有重连
    CHECK(srv_rec.connected_count() == n);
}

// 重连退避中遇到停机：不再尝试。
void TestShutdownStopsRetryingClient()
{
    const LeakCheck leak;
    EventRecorder rec;
    LengthPrefixCodecFactory factory;
    EventLoopGroup group({1, 8});
    CHECK(group.Start() == 0);
    std::atomic<int> failures{0};
    StreamClientOptions co;
    co.on_connect_failed = [&](int, bool) noexcept { failures.fetch_add(1); };
    Endpoint dead = *Endpoint::Tcp("127.0.0.1", 0);
    {
        UniqueFd fd;
        CHECK(sock::CreateStreamSocket(dead, fd) == 0 && sock::Bind(fd.get(), dead) == 0);
        std::optional<Endpoint> local;
        CHECK(sock::LocalEndpoint(fd.get(), local) == 0 && local);
        dead = *local; // 先绑后关：当下没人监听
    }
    StreamClient client(group, dead, std::move(co), ReconnectPolicy{10ms, 50ms, 2.0, 0.0}, rec, factory);
    CHECK(client.Start() == 0);
    CHECK(WaitUntil([&] { return failures.load() >= 3; }));
    group.Shutdown(500ms);
    const int n = failures.load();
    std::this_thread::sleep_for(300ms);
    CHECK(failures.load() == n);
    CHECK(rec.connected_count() == 0);
}

// Shutdown 等待期间调用方线程每轮调一次 idle：框架层借它在停机期间继续消费事件。
// 对端一直不关，直到 idle 被调过几次——证明等待期间确实在调它，且 Shutdown 一直等到对端关闭才返回（没有提前、也没等满
// deadline）。
void TestShutdownCallsIdleHookWhileWaiting()
{
    const LeakCheck leak;
    Fixture f(*Endpoint::Tcp("127.0.0.1", 0));
    TestClient c(f.server.endpoint());
    f.rec.WaitConnected(0);

    int calls = 0;
    const auto t0 = std::chrono::steady_clock::now();
    f.group.Shutdown(5s,
                     [&]
                     {
                         if (++calls == 5)
                             c.Close(); // idle 在调用 Shutdown 的线程上执行，所以可以直接操作本线程的客户端
                         return false;
                     });
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    CHECK(calls >= 5);
    CHECK(elapsed < 3s); // 对端关闭就结束，没等到 deadline
    CHECK(f.rec.closed_count() == 1 && f.rec.WaitClosed(0).reason == CloseReason::LocalClose);
}

// idle 返回 true（做了事）时不睡，立即再来。
void TestShutdownIdleHookReturningTrueDoesNotSleep()
{
    const LeakCheck leak;
    Fixture f(*Endpoint::Tcp("127.0.0.1", 0));
    TestClient c(f.server.endpoint());
    f.rec.WaitConnected(0);
    int calls = 0;
    const auto t0 = std::chrono::steady_clock::now();
    f.group.Shutdown(5s,
                     [&]
                     {
                         if (++calls == 2000)
                             c.Close();
                         return true;
                     });
    CHECK(calls >= 2000);
    CHECK(std::chrono::steady_clock::now() - t0 < 1500ms); // 返回 false 时每轮至少睡 1ms，2000 轮会超过 2s
}

// group 已经在停机 / 已停机：StreamServer / StreamClient 的 Start 返回 ESHUTDOWN，不是编程错误（回调里 Start()
// 恰好撞上停机）。
void TestStartAfterShutdownReturnsEshutdown()
{
    const LeakCheck leak;
    EventRecorder rec;
    LengthPrefixCodecFactory factory(kMaxFrame);
    EventLoopGroup group({1, 8});
    CHECK(group.Start() == 0);
    StreamServer server(group, *Endpoint::Tcp("127.0.0.1", 0), {}, rec, factory);
    StreamClient client(group, *Endpoint::Tcp("127.0.0.1", 1), {}, ReconnectPolicy{}, rec, factory);
    CHECK(!group.stopping());
    group.Shutdown(100ms);
    CHECK(group.stopping() && !group.running());
    CHECK(server.Start() == ESHUTDOWN);
    CHECK(client.Start() == ESHUTDOWN);
}

// 停机期间，在 Stopping 阶段里从另一个线程调 StreamClient::Start（模拟 on_connect_failed 回调里的 Start() 撞上停机）：
// 不崩，返回 ESHUTDOWN 或 0（赶在状态翻转之前）。
void TestStartRacingShutdownNeverCrashes()
{
    for (int round = 0; round < 20; ++round)
    {
        EventRecorder rec;
        LengthPrefixCodecFactory factory(kMaxFrame);
        EventLoopGroup group({1, 8});
        CHECK(group.Start() == 0);
        StreamClient client(
            group, *Endpoint::Tcp("127.0.0.1", 1), {}, ReconnectPolicy{20ms, 100ms, 2.0, 0.0}, rec, factory);
        std::atomic<bool> go{false};
        std::atomic<int> rc{-1};
        std::thread t(
            [&]
            {
                while (!go.load())
                    std::this_thread::yield();
                rc = client.Start();
            });
        go = true;
        group.Shutdown(50ms);
        t.join();
        CHECK(rc.load() == 0 || rc.load() == ESHUTDOWN);
    }
}

} // namespace

int main()
{
    TestShutdownDrainsGracefully(*Endpoint::Tcp("127.0.0.1", 0));
    {
        TempDir dir;
        TestShutdownDrainsGracefully(dir.Unix("d.sock"));
    }
    TestShutdownDeadlineForcesStuckConnections();
    TestShutdownZeroDeadlineClosesImmediately();
    TestNoCallbacksAfterShutdown();
    TestShutdownUnlinksUdsPath();
    TestDestructorDoesNotDrain();
    TestShutdownRacesWithAccepts();
    TestServerAndClientInOneGroupShutDownTogether();
    TestShutdownStopsRetryingClient();
    TestShutdownCallsIdleHookWhileWaiting();
    TestShutdownIdleHookReturningTrueDoesNotSleep();
    TestStartAfterShutdownReturnsEshutdown();
    TestStartRacingShutdownNeverCrashes();
    std::puts("net_lifecycle_test: ok");
    return 0;
}
