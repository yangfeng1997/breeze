// 连接级的流控与关闭：PauseRead / ResumeRead、CloseGracefully、空闲检测（假时钟，确定性）、ConnectionContext。
// 无测试框架依赖：失败即 abort。每个用例结束时检查没有 fd / 线程泄漏。

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "event_loop_group.h"
#include "group_access.h"
#include "length_prefix_codec.h"
#include "loop_server.h"
#include "net_server.h"
#include "net_test_util.h"

using namespace breeze;
using namespace breeze::testing;

namespace
{

constexpr std::size_t kMaxFrame = 8 * 1024 * 1024;

ServerOptions BigWater()
{
    ServerOptions so;
    so.connection.high_water_mark = 256ull << 20; // 这里的用例会故意制造 MB 级的写积压，水位本身由别处覆盖
    return so;
}

struct Fixture
{
    explicit Fixture(ServerOptions so = BigWater())
        : factory(kMaxFrame)
        , group({1, 64})
        , server(group, *Endpoint::Tcp("127.0.0.1", 0), std::move(so), rec, factory)
    {
        CHECK(group.Start() == 0);
        CHECK(server.Start() == 0);
    }
    std::uint64_t Paused() { return group.GetStats().paused_connections; }

    EventRecorder rec;
    LengthPrefixCodecFactory factory;
    EventLoopGroup group;
    NetServer server;
};

// ---- PauseRead / ResumeRead ----

// 暂停之后不再交付，恢复之后补上，一条不少、顺序不乱。
void TestPauseHoldsBackDeliveryUntilResume()
{
    const LeakCheck leak;
    Fixture f;
    f.rec.on_message = [](Connection& c, std::string_view s)
    {
        if (s == "pause-me")
            c.PauseRead();
    };
    TestClient c(f.server.endpoint());
    const ConnectionHandle h = f.rec.WaitConnected(0);
    c.SendFrame("m0");
    c.SendFrame("pause-me");
    CHECK(f.rec.WaitMessages(2));
    CHECK(WaitUntil([&] { return f.Paused() == 1; }));

    for (int i = 2; i < 10; ++i)
        c.SendFrame("m" + std::to_string(i));
    std::this_thread::sleep_for(150ms);
    CHECK(f.rec.message_count() == 2); // 暂停期间一条也没交付

    h.ResumeRead();
    h.ResumeRead(); // 重复是空操作
    CHECK(f.rec.WaitMessages(10));
    const auto got = f.rec.payloads();
    CHECK(got[0] == "m0" && got[1] == "pause-me");
    for (int i = 2; i < 10; ++i)
        CHECK(got[static_cast<std::size_t>(i)] == "m" + std::to_string(i));
    CHECK(WaitUntil([&] { return f.Paused() == 0; }));
    f.rec.CheckInvariants();
}

// 反复暂停 / 恢复，大量消息不丢、不乱序。
void TestPauseResumeKeepsOrder()
{
    const LeakCheck leak;
    Fixture f;
    constexpr int kN = 3000;
    std::atomic<int> seen{0};
    f.rec.on_message = [&](Connection& c, std::string_view)
    {
        if (seen.fetch_add(1) % 100 == 99)
            c.PauseRead();
    };
    TestClient c(f.server.endpoint());
    const ConnectionHandle h = f.rec.WaitConnected(0);

    std::thread sender(
        [&]
        {
            for (int i = 0; i < kN; ++i)
                c.SendFrame(std::to_string(i));
        });
    while (f.rec.message_count() < kN)
    {
        if (f.Paused() > 0)
            h.ResumeRead();
        std::this_thread::sleep_for(200us);
    }
    sender.join();
    const auto got = f.rec.payloads();
    CHECK(got.size() == kN);
    for (int i = 0; i < kN; ++i)
        CHECK(got[static_cast<std::size_t>(i)] == std::to_string(i));
}

// 暂停着的连接遇到 RST：EPOLLERR / EPOLLHUP 内核总会报，照样被关掉，暂停计数归零。
void TestPausedConnectionStillClosesOnReset()
{
    const LeakCheck leak;
    Fixture f;
    f.rec.on_message = [](Connection& c, std::string_view) { c.PauseRead(); };
    TestClient c(f.server.endpoint());
    c.SendFrame("x");
    CHECK(WaitUntil([&] { return f.Paused() == 1; }));
    c.ArmReset();
    c.Close();
    const auto closed = f.rec.WaitClosed(0);
    CHECK(closed.reason == CloseReason::IoError || closed.reason == CloseReason::PeerClosed);
    CHECK(WaitUntil([&] { return f.Paused() == 0; }));
    f.rec.CheckInvariants();
}

// 暂停着的连接对端正常关闭（FIN）：暂停时连 EPOLLRDHUP 也摘了（否则水平触发空转），所以要到恢复读才发现。
void TestPausedConnectionNoticesFinOnResume()
{
    const LeakCheck leak;
    Fixture f;
    f.rec.on_message = [](Connection& c, std::string_view) { c.PauseRead(); };
    TestClient c(f.server.endpoint());
    const ConnectionHandle h = f.rec.WaitConnected(0);
    c.SendFrame("x");
    CHECK(WaitUntil([&] { return f.Paused() == 1; }));
    EventLoop& loop = GroupAccess::LoopAt(f.group, 0);
    const std::uint64_t iters_before = loop.Iterations();
    c.Close();
    std::this_thread::sleep_for(150ms);
    CHECK(f.rec.closed_count() == 0);
    CHECK(loop.Iterations() - iters_before < 20); // 没有空转
    h.ResumeRead();
    CHECK(f.rec.WaitClosed(0).reason == CloseReason::PeerClosed);
}

void TestPauseOnClosedHandleIsNoop()
{
    const LeakCheck leak;
    Fixture f;
    TestClient c(f.server.endpoint());
    const ConnectionHandle h = f.rec.WaitConnected(0);
    h.Close();
    f.rec.WaitClosed(0);
    h.PauseRead();
    h.ResumeRead();
    h.CloseGracefully(10ms);
    std::this_thread::sleep_for(50ms);
    CHECK(f.rec.closed_count() == 1 && f.Paused() == 0);
}

// ---- CloseGracefully ----

// 队列里的数据先发完，再 shutdown(SHUT_WR)；对端收到全部数据与 EOF，关闭之后正常结束（LocalClose），不用等超时。
void TestGracefulFlushesThenWaitsForPeer()
{
    const LeakCheck leak;
    Fixture f;
    constexpr std::size_t kBig = 3 * 1024 * 1024; // 远超内核缓冲：一部分必然在写队列里
    f.rec.on_message = [&](Connection& c, std::string_view)
    {
        SharedBuffer b = SharedBuffer::Create(kBig);
        std::memset(b.MutableBytes().data(), 'g', kBig);
        CHECK(c.Send(std::move(b)) == SendResult::Ok);
        c.CloseGracefully(5s);
        CHECK(!c.IsConnected());
        CHECK(c.Send(Buf("after")) == SendResult::Closed); // Disconnecting：不再接受 Send
    };
    TestClient c(f.server.endpoint());
    const ConnectionHandle h = f.rec.WaitConnected(0);
    c.SendFrame("go");
    const std::string big = c.RecvFrame();
    CHECK(big.size() == kBig && big.front() == 'g' && big.back() == 'g'); // 最后一条不丢
    CHECK(c.WaitClosed());                                                // 之后是 EOF
    CHECK(f.rec.closed_count() == 0);                                     // 服务端在等对端关闭，没有提前结束
    CHECK(h.Send(Buf("x")) == SendResult::Closed);                        // 跨线程的 Send 同样得到 Closed

    const auto t0 = std::chrono::steady_clock::now();
    c.Close();
    const auto closed = f.rec.WaitClosed(0);
    CHECK(closed.reason == CloseReason::LocalClose);
    CHECK(std::chrono::steady_clock::now() - t0 < 2s); // 对端关闭就结束，没等 5s 超时
    f.rec.CheckInvariants();
}

// 对端读完、收到 EOF 却一直不关：到 timeout 强关，原因 LocalClose。
void TestGracefulForceClosesOnTimeout()
{
    const LeakCheck leak;
    Fixture f;
    f.rec.on_message = [](Connection& c, std::string_view)
    {
        CHECK(c.Send(Buf("bye")) == SendResult::Ok);
        c.CloseGracefully(300ms);
    };
    TestClient c(f.server.endpoint());
    const auto t0 = std::chrono::steady_clock::now();
    c.SendFrame("go");
    CHECK(c.RecvFrame() == "bye");
    CHECK(c.WaitClosed()); // EOF
    CHECK(f.rec.closed_count() == 0);
    const auto closed = f.rec.WaitClosed(0);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    CHECK(closed.reason == CloseReason::LocalClose);
    CHECK(elapsed >= 250ms && elapsed < 3s);
    std::this_thread::sleep_for(50ms);
    CHECK(f.rec.closed_count() == 1);
}

// 对端根本不读：积压排不空，到 timeout 同样强关。
void TestGracefulForceClosesWithUnsentBacklog()
{
    const LeakCheck leak;
    Fixture f;
    TestClient c(f.server.endpoint()); // 不读
    const ConnectionHandle h = f.rec.WaitConnected(0);
    for (int i = 0; i < 4; ++i)
        CHECK(h.Send(SharedBuffer::Create(2 * 1024 * 1024)) == SendResult::Ok);
    const auto t0 = std::chrono::steady_clock::now();
    h.CloseGracefully(200ms);
    const auto closed = f.rec.WaitClosed(0);
    CHECK(closed.reason == CloseReason::LocalClose);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    CHECK(elapsed >= 150ms && elapsed < 3s);
}

// 优雅关闭期间仍然读：对端在 FIN 之前发来的最后几条消息要交付，且在 OnClosed 之前。
void TestGracefulStillDeliversPeerMessages()
{
    const LeakCheck leak;
    Fixture f;
    f.rec.on_message = [](Connection& c, std::string_view s)
    {
        if (s == "go")
            c.CloseGracefully(5s);
    };
    TestClient c(f.server.endpoint());
    c.SendFrame("go");
    CHECK(c.WaitClosed()); // 服务端队列为空：立刻 shutdown(SHUT_WR)
    c.SendFrame("last");   // 我们这边还能发
    CHECK(f.rec.WaitMessages(2));
    CHECK(f.rec.payloads()[1] == "last");
    c.Close();
    CHECK(f.rec.WaitClosed(0).reason == CloseReason::LocalClose);
    f.rec.CheckInvariants(); // OnClosed 在最后一个 OnMessage 之后
}

// 经句柄：此前同一线程 Send 的数据先发完，再关。
void TestGracefulViaHandleKeepsOrder()
{
    const LeakCheck leak;
    Fixture f;
    TestClient c(f.server.endpoint());
    const ConnectionHandle h = f.rec.WaitConnected(0);
    CHECK(h.Send(Buf("a")) == SendResult::Ok);
    CHECK(h.Send(Buf("b")) == SendResult::Ok);
    h.CloseGracefully(5s);
    CHECK(c.RecvFrame() == "a");
    CHECK(c.RecvFrame() == "b");
    CHECK(c.WaitClosed());
    c.Close();
    CHECK(f.rec.WaitClosed(0).reason == CloseReason::LocalClose);
}

// 暂停着的连接做优雅关闭：自动恢复读，对端在 FIN 之前发来的最后几条消息才读得到。
// （没有恢复读也能发现对端关闭——我方 SHUT_WR 之后对端再 FIN，两个方向都关了，内核会报 EPOLLHUP——但那样会直接关掉，
// 来不及读的数据就丢了。）
void TestGracefulResumesReadingToDeliverPeerMessages()
{
    const LeakCheck leak;
    Fixture f;
    f.rec.on_message = [](Connection& c, std::string_view s)
    {
        if (s == "pause")
            c.PauseRead();
    };
    TestClient c(f.server.endpoint());
    const ConnectionHandle h = f.rec.WaitConnected(0);
    c.SendFrame("pause");
    CHECK(WaitUntil([&] { return f.Paused() == 1; }));
    h.CloseGracefully(5s);
    CHECK(c.WaitClosed()); // 服务端队列为空：立刻 SHUT_WR
    CHECK(WaitUntil([&] { return f.Paused() == 0; }));
    c.SendFrame("last");
    CHECK(f.rec.WaitMessages(2));
    CHECK(f.rec.payloads()[1] == "last");
    const auto t0 = std::chrono::steady_clock::now();
    c.Close();
    CHECK(f.rec.WaitClosed(0).reason == CloseReason::LocalClose);
    CHECK(std::chrono::steady_clock::now() - t0 < 2s);
    f.rec.CheckInvariants();
}

// 连接关闭时 grace 定时器要取消：它捕获着 this，连接对象销毁后再触发就是悬空；堆块被新连接复用的话，还会误关新连接。
void TestStaleGraceTimerDoesNotTouchReusedConnection()
{
    const LeakCheck leak;
    Fixture f;
    TestClient a(f.server.endpoint());
    const ConnectionHandle ha = f.rec.WaitConnected(0);
    ha.CloseGracefully(300ms);
    CHECK(a.WaitClosed());
    a.Close(); // 对端很快关闭：在 300ms 之前正常结束
    CHECK(f.rec.WaitClosed(0).reason == CloseReason::LocalClose);
    CHECK(WaitUntil([&] { return f.group.GetStats().connections == 0; }));

    TestClient b(f.server.endpoint()); // 同一大小的对象，glibc 很可能复用刚释放的那一块
    f.rec.WaitConnected(1);
    std::this_thread::sleep_for(600ms); // 旧定时器（300ms）如果还在，此刻已经触发过
    CHECK(f.rec.closed_count() == 1);   // B 还活着
    f.rec.on_message = Echo;
    b.SendFrame("alive");
    CHECK(b.RecvFrame() == "alive");
}

// ---- 空闲检测：假时钟，一步 100ms，不依赖真实时间 ----

struct FakeLoop
{
    explicit FakeLoop(std::uint32_t max_connections = 16)
        : loop([this] { return now; })
    {
        CHECK(loop.Init({LoopOptions::kNoGroup, 0, max_connections}) == 0);
    }

    // 不动时钟，跑 loop 直到 pred 成立（等内核把字节递到）。
    template <typename Pred>
    void PumpUntil(Pred pred)
    {
        const auto deadline = std::chrono::steady_clock::now() + kWait;
        while (!pred())
        {
            CHECK(std::chrono::steady_clock::now() < deadline);
            loop.RunOnce(0);
            std::this_thread::sleep_for(500us);
        }
    }
    // 假时钟前进 d，每 100ms 跑一轮。
    void Advance(std::chrono::milliseconds d)
    {
        for (; d.count() > 0; d -= 100ms)
        {
            now += 100ms;
            loop.RunOnce(0);
        }
    }

    EventLoop::Clock::time_point now = EventLoop::Clock::time_point{} + 1000s;
    EventLoop loop;
};

// 时间点的排法：连接占 0 号槽，空闲扫描每 100ms 一次、每次扫 2 个槽（16 槽 / 10 份向上取整），所以 0 号槽在
// t0+100、+900、+1700、+2500、+3300、+4100…被检查一次（一圈 800ms）。idle_timeout 取 2000ms（大于一圈），
// 这样"刷新过 last_active"与"没刷新"在检查点上结果不同，而不是碰运气。
void TestIdleTimeoutFiresAndActivityPostpones()
{
    const LeakCheck leak;
    FakeLoop fl;
    EventRecorder rec;
    LengthPrefixCodecFactory factory;
    ConnectionOptions co;
    co.idle_timeout = 2000ms;
    LoopServer srv(fl.loop, *Endpoint::Tcp("127.0.0.1", 0), rec, factory, co);
    CHECK(srv.Start() == 0);
    TestClient c(srv.endpoint());
    fl.PumpUntil([&] { return rec.connected_count() == 1; });
    CHECK(rec.WaitConnected(0).slot() == 0);

    fl.Advance(1500ms);
    CHECK(rec.closed_count() == 0); // 还没到 2000ms
    c.SendFrame("alive");
    fl.PumpUntil([&] { return rec.message_count() == 1; }); // 收到字节：last_active 刷新到 t0+1500
    fl.Advance(1500ms);                                     // t0+3000：没刷新的话 t0+2500 那次检查就关了
    CHECK(rec.closed_count() == 0);
    fl.Advance(1500ms); // t0+4500：t0+4100 那次检查时已空闲 2600ms ≥ 2000ms
    CHECK(rec.closed_count() == 1);
    CHECK(rec.WaitClosed(0).reason == CloseReason::IdleTimeout);
    rec.CheckInvariants();
}

// 暂停期间不计空闲；恢复读时重新计时。
void TestPausedConnectionIsNotIdleKilled()
{
    const LeakCheck leak;
    FakeLoop fl;
    EventRecorder rec;
    Connection* conn = nullptr;
    rec.on_connected = [&](Connection& c) { conn = &c; };
    rec.on_message = [](Connection& c, std::string_view) { c.PauseRead(); };
    LengthPrefixCodecFactory factory;
    ConnectionOptions co;
    co.idle_timeout = 2000ms;
    LoopServer srv(fl.loop, *Endpoint::Tcp("127.0.0.1", 0), rec, factory, co);
    CHECK(srv.Start() == 0);
    TestClient c(srv.endpoint());
    fl.PumpUntil([&] { return rec.connected_count() == 1; });
    c.SendFrame("pause");
    fl.PumpUntil([&] { return rec.message_count() == 1; });
    CHECK(fl.loop.stats().paused.load() == 1);

    fl.Advance(10s); // 远超 idle_timeout 与一圈扫描：后端过载时连接被长时间暂停，不能被集体误杀
    CHECK(rec.closed_count() == 0);

    conn->ResumeRead();
    fl.Advance(1000ms); // 一圈 800ms：0 号槽至少被检查一次；恢复时刷新了 last_active，所以此刻只空闲了 ≤ 1000ms
    CHECK(rec.closed_count() == 0);
    fl.Advance(3000ms); // 之后真的空闲超过 2000ms
    CHECK(rec.closed_count() == 1);
    CHECK(rec.WaitClosed(0).reason == CloseReason::IdleTimeout);
    CHECK(fl.loop.stats().paused.load() == 0);
}

void TestIdleOffByDefault()
{
    const LeakCheck leak;
    FakeLoop fl;
    EventRecorder rec;
    LengthPrefixCodecFactory factory;
    LoopServer srv(fl.loop, *Endpoint::Tcp("127.0.0.1", 0), rec, factory); // idle_timeout 默认 0
    CHECK(srv.Start() == 0);
    TestClient c(srv.endpoint());
    fl.PumpUntil([&] { return rec.connected_count() == 1; });
    fl.Advance(std::chrono::seconds(600));
    CHECK(rec.closed_count() == 0);
}

// 连接关闭后注销：没有登记的连接时扫描定时器不再重装；槽复用给没开 idle 的连接也不会被误杀。
void TestIdleEntryClearedOnClose()
{
    const LeakCheck leak;
    FakeLoop fl(2);
    EventRecorder rec;
    LengthPrefixCodecFactory factory;
    ConnectionOptions co;
    co.idle_timeout = 300ms;
    LoopServer srv(fl.loop, *Endpoint::Tcp("127.0.0.1", 0), rec, factory, co);
    CHECK(srv.Start() == 0);
    {
        TestClient c(srv.endpoint());
        fl.PumpUntil([&] { return rec.connected_count() == 1; });
        c.Close();
        fl.PumpUntil([&] { return rec.closed_count() == 1; });
    }
    CHECK(rec.WaitClosed(0).reason == CloseReason::PeerClosed);
    fl.Advance(5s); // 之后什么也不会再被扫到关掉
    CHECK(rec.closed_count() == 1);
}

// ---- ConnectionContext ----

struct Ctx final : ConnectionContext
{
    explicit Ctx(std::atomic<bool>& d)
        : destroyed(d)
    {
    }
    ~Ctx() override { destroyed = true; }
    int n = 0;
    std::atomic<bool>& destroyed;
};

void TestContextLivesUntilAfterOnClosed()
{
    const LeakCheck leak;
    Fixture f;
    std::atomic<bool> destroyed{false};
    std::atomic<int> seen_in_closed{-1};
    f.rec.on_connected = [&](Connection& c) { c.SetContext(std::make_unique<Ctx>(destroyed)); };
    f.rec.on_message = [](Connection& c, std::string_view) { c.GetContext<Ctx>()->n++; };
    f.rec.on_closed = [&](Connection& c, CloseReason)
    {
        CHECK(!destroyed.load()); // OnClosed 时上下文还在
        seen_in_closed = c.GetContext<Ctx>()->n;
    };
    {
        TestClient c(f.server.endpoint());
        for (int i = 0; i < 3; ++i)
            c.SendFrame("m");
        CHECK(f.rec.WaitMessages(3));
    }
    CHECK(f.rec.WaitClosed(0).reason == CloseReason::PeerClosed);
    CHECK(seen_in_closed.load() == 3);
    CHECK(WaitUntil([&] { return destroyed.load(); })); // 连接销毁时一起析构
}

} // namespace

int main()
{
    TestPauseHoldsBackDeliveryUntilResume();
    TestPauseResumeKeepsOrder();
    TestPausedConnectionStillClosesOnReset();
    TestPausedConnectionNoticesFinOnResume();
    TestPauseOnClosedHandleIsNoop();
    TestGracefulFlushesThenWaitsForPeer();
    TestGracefulForceClosesOnTimeout();
    TestGracefulForceClosesWithUnsentBacklog();
    TestGracefulStillDeliversPeerMessages();
    TestGracefulViaHandleKeepsOrder();
    TestGracefulResumesReadingToDeliverPeerMessages();
    TestStaleGraceTimerDoesNotTouchReusedConnection();
    TestIdleTimeoutFiresAndActivityPostpones();
    TestPausedConnectionIsNotIdleKilled();
    TestIdleOffByDefault();
    TestIdleEntryClearedOnClose();
    TestContextLivesUntilAfterOnClosed();
    std::puts("net_flow_test: ok");
    return 0;
}
