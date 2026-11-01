// MainLoopExecutor：事件在主循环线程上按连接全序分发、Drain 的返回值与轮转、payload 的所有权、
// 每连接 / 全局的入口背压（上界、恢复、不丢不乱序），以及小限额下的并发压力（专打"丢恢复"这类竞态）。
// 无测试框架依赖：失败即 abort；并发用例带超时，卡死（比如丢了恢复）也会 abort 而不是挂住 ctest。

#include "framework/net/main_loop_executor.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "base/net/codec/length_prefix_codec.h"
#include "base/net/loop/event_loop_group.h"
#include "base/net/stream/stream_server.h"
#include "base/net/testing/net_test_util.h"
#include "framework/app/app.h"

using namespace breeze;
using namespace breeze::testing;

namespace
{

constexpr std::size_t kMaxFrame = 1 << 20;

// 记录主循环上收到的事件。所有回调都应当在同一个线程（调 Drain 的线程）上。
class Recorder final : public IMainLoopHandler
{
public:
    struct Ev
    {
        char kind; // C / M / X
        ConnectionHandle handle;
        std::string payload;
        CloseReason reason = CloseReason::LocalClose;
        bool has_peer = false;
    };

    void OnConnected(ConnectionHandle h, const PeerInfo& p) override
    {
        Check();
        events.push_back({'C', h, {}, CloseReason::LocalClose, p.endpoint.has_value()});
    }
    void OnMessage(ConnectionHandle h, SharedBuffer msg) override
    {
        Check();
        events.push_back({'M', h, Str(msg.bytes())});
        kept.push_back(std::move(msg)); // 业务可以保存：回调返回之后仍然有效
        if (on_message)
            on_message(h);
    }
    void OnClosed(ConnectionHandle h, CloseReason r, int) override
    {
        Check();
        events.push_back({'X', h, {}, r});
    }

    std::size_t Count(char kind) const
    {
        std::size_t n = 0;
        for (const auto& e : events)
            n += e.kind == kind;
        return n;
    }

    std::vector<Ev> events;
    std::vector<SharedBuffer> kept;
    std::function<void(ConnectionHandle)> on_message;
    std::thread::id main_thread = std::this_thread::get_id();

private:
    void Check() const { CHECK(std::this_thread::get_id() == main_thread); }
};

InboundLimits Limits(std::size_t msgs, std::size_t bytes, std::size_t global)
{
    InboundLimits l;
    l.max_messages_per_conn = msgs;
    l.max_bytes_per_conn = bytes;
    l.global_budget_bytes = global;
    return l;
}

StreamServerOptions BigWater()
{
    StreamServerOptions so;
    so.connection.high_water_mark = 256ull << 20;
    return so;
}

struct Fixture
{
    explicit Fixture(InboundLimits lim = {}, EventLoopGroupOptions go = {2, 256})
        : app("exec-test")
        , executor(app, lim)
        , factory(kMaxFrame)
        , group(go)
        , server(group, *Endpoint::Tcp("127.0.0.1", 0), BigWater(), executor.Wrap(handler), factory)
    {
        CHECK(group.Start() == 0);
        CHECK(server.Start() == 0);
    }

    // 主循环线程（本线程）一直 Drain，直到 pred 成立。
    // spin：空了也不睡，紧跟着 IO 线程的节奏——压力用例要把"主线程恰好在 IO 线程两步之间消费"的窗口撑开。
    template <typename Pred>
    bool DrainUntil(Pred pred, std::chrono::milliseconds timeout = kWait, bool spin = false)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!pred())
        {
            if (std::chrono::steady_clock::now() > deadline)
                return false;
            if (!executor.Drain(256) && !spin)
                std::this_thread::sleep_for(200us);
        }
        return true;
    }
    std::uint64_t Paused() { return group.GetStats().paused_connections; }

    App app;
    MainLoopExecutor executor;
    Recorder handler;
    LengthPrefixCodecFactory factory;
    EventLoopGroup group;
    StreamServer server;
};

// ---- 事件与所有权 ----

void TestEventsArriveInOrderOnMainLoop()
{
    const LeakCheck leak;
    Fixture f;
    {
        TestClient c(f.server.endpoint());
        for (int i = 0; i < 5; ++i)
            c.SendFrame("m" + std::to_string(i));
        CHECK(WaitUntil([&] { return f.executor.GetStats().queued_events == 6; })); // C + 5 M，关闭之前
    }
    CHECK(f.DrainUntil([&] { return f.handler.Count('X') == 1; }));

    const auto& ev = f.handler.events;
    CHECK(ev.size() == 7);
    CHECK(ev.front().kind == 'C' && ev.front().has_peer);
    for (int i = 0; i < 5; ++i)
        CHECK(ev[static_cast<std::size_t>(i) + 1].kind == 'M' &&
              ev[static_cast<std::size_t>(i) + 1].payload == "m" + std::to_string(i));
    CHECK(ev.back().kind == 'X' && ev.back().reason == CloseReason::PeerClosed);
    for (const auto& e : ev)
        CHECK(e.handle == ev.front().handle && e.handle.valid());

    // 业务保存的 msg 在回调返回之后仍然有效，内容没被后续读覆盖
    CHECK(f.handler.kept.size() == 5);
    for (int i = 0; i < 5; ++i)
        CHECK(Str(f.handler.kept[static_cast<std::size_t>(i)].bytes()) == "m" + std::to_string(i));
    CHECK(f.executor.GetStats().inflight_bytes == 0);
}

// Drain 的返回值：分发了 limit 个就可能还有，不足 limit 一定已空。
void TestDrainReturnValueAndLimit()
{
    const LeakCheck leak;
    Fixture f;
    TestClient c(f.server.endpoint());
    for (int i = 0; i < 3; ++i)
        c.SendFrame("x");
    CHECK(WaitUntil([&] { return f.executor.GetStats().queued_events == 4; })); // C + 3 M
    CHECK(f.executor.Drain(2));                                                 // 取满 2 个：可能还有
    CHECK(f.handler.events.size() == 2);
    CHECK(f.executor.Drain(1));
    CHECK(f.handler.events.size() == 3);
    CHECK(!f.executor.Drain(100)); // 只剩 1 个，不足 100：已空
    CHECK(f.handler.events.size() == 4);
    CHECK(!f.executor.Drain(100));
    const ExecutorStats s = f.executor.GetStats();
    CHECK(s.dispatched == 4 && s.queued_events == 0 && s.queue_peak >= 4);
}

// 一次只放行一个事件，Closed 不会超到还没分发的 Message 前面。
void TestClosedNeverOvertakes()
{
    const LeakCheck leak;
    Fixture f;
    {
        TestClient c(f.server.endpoint());
        for (int i = 0; i < 20; ++i)
            c.SendFrame(std::to_string(i));
    }
    CHECK(WaitUntil([&] { return f.executor.GetStats().queued_events == 22; })); // C + 20 M + X
    while (f.executor.Drain(1))
    {
    }
    const auto& ev = f.handler.events;
    CHECK(ev.size() == 22);
    CHECK(ev.front().kind == 'C' && ev.back().kind == 'X');
    for (int i = 0; i < 20; ++i)
        CHECK(ev[static_cast<std::size_t>(i) + 1].payload == std::to_string(i));
}

// 多条连接分在多个 loop（多条队列）上：各连接内部有序，队列之间轮转，谁也不被饿死。
void TestManyConnectionsAcrossQueuesKeepPerConnectionOrder()
{
    const LeakCheck leak;
    Fixture f({}, {4, 64});
    constexpr int kConns = 8;
    constexpr int kMsgs = 200;
    std::vector<std::unique_ptr<TestClient>> cs;
    for (int i = 0; i < kConns; ++i)
        cs.push_back(std::make_unique<TestClient>(f.server.endpoint()));
    std::vector<std::thread> senders;
    for (int i = 0; i < kConns; ++i)
    {
        senders.emplace_back(
            [&, i]
            {
                for (int n = 0; n < kMsgs; ++n)
                    cs[static_cast<std::size_t>(i)]->SendFrame("c" + std::to_string(i) + ":" + std::to_string(n));
            });
    }
    CHECK(f.DrainUntil([&] { return f.handler.Count('M') == kConns * kMsgs; }));
    for (auto& t : senders)
        t.join();

    std::unordered_map<ConnectionHandle, std::vector<std::string>> per;
    for (const auto& e : f.handler.events)
        if (e.kind == 'M')
            per[e.handle].push_back(e.payload);
    CHECK(per.size() == kConns);
    for (const auto& [h, msgs] : per)
    {
        CHECK(msgs.size() == kMsgs);
        const std::string prefix = msgs[0].substr(0, msgs[0].find(':'));
        for (int n = 0; n < kMsgs; ++n)
            CHECK(msgs[static_cast<std::size_t>(n)] == prefix + ":" + std::to_string(n));
    }
    // 事件队列不止一条：Connected 也要在各自连接的第一个 Message 之前
    std::unordered_map<ConnectionHandle, int> seen;
    for (const auto& e : f.handler.events)
    {
        if (e.kind == 'C')
            CHECK(seen[e.handle]++ == 0);
        else
            CHECK(seen[e.handle] > 0);
    }
}

// ---- 入口背压 ----

// 每连接在途上限：主循环不 Drain 时，在途量被钉在 上限 + 一次 read（≤64KB）之内，其余留在内核缓冲里；
// 开始 Drain 之后恢复，所有消息一条不少、顺序不乱。
void TestPerConnectionBackpressure()
{
    const LeakCheck leak;
    constexpr std::size_t kFrame = 4096;
    constexpr int kFrames = 400; // 1.6MB
    constexpr std::size_t kBytesLimit = 16 * 1024;
    Fixture f(Limits(8, kBytesLimit, 1ull << 30));
    TestClient c(f.server.endpoint());
    std::thread sender(
        [&]
        {
            for (int i = 0; i < kFrames; ++i)
                c.SendFrame(std::string(kFrame - 7, 'x') + std::to_string(1000000 + i)); // 末尾 7 位是递增序号
        });

    CHECK(WaitUntil([&] { return f.Paused() == 1; }));
    std::this_thread::sleep_for(150ms);
    const ExecutorStats mid = f.executor.GetStats();
    CHECK(mid.pauses >= 1);
    CHECK(mid.inflight_bytes <= kBytesLimit + 64 * 1024 + kFrame);            // 软上限：最多超出一次 read
    CHECK(mid.inflight_bytes < static_cast<std::uint64_t>(kFrame) * kFrames); // 其余留在内核缓冲里

    CHECK(f.DrainUntil([&] { return f.handler.Count('M') == kFrames; }));
    sender.join();
    // 顺序：每条消息的末尾是递增的序号
    std::uint64_t last = 0;
    for (const auto& e : f.handler.events)
    {
        if (e.kind != 'M')
            continue;
        const std::uint64_t v = std::stoull(e.payload.substr(e.payload.size() - 7));
        CHECK(v == 1000000 + last);
        ++last;
    }
    CHECK(last == kFrames);
    CHECK(WaitUntil([&] { return f.Paused() == 0; })); // 恢复了
    CHECK(f.executor.GetStats().inflight_bytes == 0);
}

// 全局预算：每连接上限很大，但所有连接在途字节之和受预算约束；降下来后统一恢复，没有连接被遗忘。
void TestGlobalBudgetPausesAllAndResumesAll()
{
    const LeakCheck leak;
    constexpr std::size_t kFrame = 4096;
    constexpr int kConns = 3;
    constexpr int kFrames = 100;
    constexpr std::size_t kBudget = 64 * 1024;
    Fixture f(Limits(1 << 20, 1 << 30, kBudget), {2, 64});
    std::vector<std::unique_ptr<TestClient>> cs;
    for (int i = 0; i < kConns; ++i)
        cs.push_back(std::make_unique<TestClient>(f.server.endpoint()));
    std::vector<std::thread> senders;
    for (int i = 0; i < kConns; ++i)
    {
        senders.emplace_back(
            [&, i]
            {
                for (int n = 0; n < kFrames; ++n)
                    cs[static_cast<std::size_t>(i)]->SendFrame("c" + std::to_string(i) + ":" + std::to_string(n) + ":" +
                                                               std::string(kFrame, 'y'));
            });
    }
    CHECK(WaitUntil([&] { return f.Paused() == kConns; })); // 三条连接都被暂停
    std::this_thread::sleep_for(150ms);
    CHECK(f.executor.GetStats().inflight_bytes <= kBudget + kConns * (64 * 1024 + kFrame + 16));

    CHECK(f.DrainUntil([&] { return f.handler.Count('M') == kConns * kFrames; }));
    for (auto& t : senders)
        t.join();
    std::unordered_map<ConnectionHandle, int> next;
    for (const auto& e : f.handler.events)
    {
        if (e.kind != 'M')
            continue;
        const auto first = e.payload.find(':');
        const auto second = e.payload.find(':', first + 1);
        CHECK(std::stoi(e.payload.substr(first + 1, second - first - 1)) == next[e.handle]++);
    }
    CHECK(WaitUntil([&] { return f.Paused() == 0; }));
    CHECK(f.executor.GetStats().inflight_bytes == 0);
}

// 小限额下的并发压力：IO 线程暂停 / 主线程恢复来回很多次，不能丢恢复（丢了就是连接永久暂停，下面会超时 abort），
// 不丢消息、不乱序。三组限额分别压 "只有每连接上限"、"只有全局预算"、"两者都小"。
void RunStress(const char* label, InboundLimits lim, bool spin = false, int conns = 8, int msgs = 1500)
{
    const LeakCheck leak;
    const int kConns = conns;
    const int kMsgs = msgs;
    Fixture f(lim, {4, 64});
    std::vector<std::unique_ptr<TestClient>> cs;
    for (int i = 0; i < kConns; ++i)
        cs.push_back(std::make_unique<TestClient>(f.server.endpoint()));
    std::vector<std::thread> senders;
    for (int i = 0; i < kConns; ++i)
    {
        senders.emplace_back(
            [&, i]
            {
                for (int n = 0; n < kMsgs; ++n)
                    cs[static_cast<std::size_t>(i)]->SendFrame("c" + std::to_string(i) + ":" + std::to_string(n) + ":" +
                                                               std::string(100, 'z'));
            });
    }
    if (!f.DrainUntil([&] { return f.handler.Count('M') == static_cast<std::size_t>(kConns * kMsgs); }, 30s, spin))
    {
        std::fprintf(stderr,
                     "压力用例 [%s] 超时：收到 %zu / %d，暂停中 %llu\n",
                     label,
                     f.handler.Count('M'),
                     kConns * kMsgs,
                     static_cast<unsigned long long>(f.Paused()));
        std::abort();
    }
    for (auto& t : senders)
        t.join();
    std::unordered_map<ConnectionHandle, int> next;
    for (const auto& e : f.handler.events)
    {
        if (e.kind != 'M')
            continue;
        const auto first = e.payload.find(':');
        const auto second = e.payload.find(':', first + 1);
        CHECK(std::stoi(e.payload.substr(first + 1, second - first - 1)) == next[e.handle]++);
    }
    CHECK(WaitUntil([&] { return f.Paused() == 0; }));
    CHECK(f.executor.GetStats().inflight_bytes == 0);
    std::printf("main_loop_executor_test: 压力 [%s] pauses=%llu\n",
                label,
                static_cast<unsigned long long>(f.executor.GetStats().pauses));
}

void TestBackpressureStress()
{
    RunStress("每连接", Limits(2, 1024, 1ull << 30));
    RunStress("全局", Limits(1 << 20, 1 << 30, 8 * 1024));
    RunStress("两者", Limits(2, 1024, 8 * 1024));
    // 最凶的一组：每连接只许 1 条在途、主线程自旋，IO 线程每条消息都在暂停边缘，主线程几乎总是"恰好在 IO
    // 线程两步之间"消费。
    RunStress("极限·每连接", Limits(1, 1 << 30, 1ull << 30), /*spin=*/true, 16, 1500);
    RunStress("极限·两者", Limits(1, 1 << 30, 2 * 1024), /*spin=*/true, 16, 1500);
}

// 滞后区间：高水位 8 条、低水位 4 条。暂停之后一条条消费，降到 4 条那一刻才恢复，5 条时必须仍暂停——
// 没有滞后的话在高水位附近每条消息都暂停 / 恢复一次，epoll_ctl 来回。
void TestHysteresisBetweenHighAndLowWater()
{
    const LeakCheck leak;
    Fixture f(Limits(8, 1ull << 30, 1ull << 30));
    TestClient c(f.server.endpoint());
    std::vector<std::byte> batch; // 20 帧拼成一次 send：服务端一次 read 全部读进来，在途一下子到 20
    for (int i = 0; i < 20; ++i)
    {
        const auto fr = TestClient::Frame("0123456789");
        batch.insert(batch.end(), fr.begin(), fr.end());
    }
    c.SendAll(batch);
    CHECK(WaitUntil([&] { return f.executor.GetStats().queued_events == 21; })); // C + 20 M
    CHECK(WaitUntil([&] { return f.Paused() == 1; }));

    CHECK(f.executor.Drain(1)); // Connected
    for (int consumed = 1; consumed <= 20; ++consumed)
    {
        CHECK(f.executor.Drain(1));
        const int inflight = 20 - consumed;
        if (inflight > 4)
        {
            std::this_thread::sleep_for(20ms); // 让可能已经投递的 ResumeRead 有机会执行
            CHECK(f.Paused() == 1);            // 高于低水位：仍暂停
        }
        else if (inflight == 4)
        {
            CHECK(WaitUntil([&] { return f.Paused() == 0; })); // 降到低水位：恢复
        }
    }
    CHECK(f.handler.Count('M') == 20);
}

// 过期的 ResumeRead 不能让暂停标志与物理状态脱节（独立审查指出的交错）：
//   1. IO 线程在一次 read 的第 9 帧处暂停（标志 true、物理暂停）；
//   2. 主线程这时把积压消费光 → 标志清掉、投递 ResumeRead（还没执行：IO 线程还在分发同一次 read 剩下的帧）；
//   3. IO 线程继续分发，在途又超过高水位：此时物理上仍是暂停，翻转标志再 PauseRead 是空操作，随后过期的 ResumeRead
//      生效——连接物理上恢复读，标志却是 true，MaybePause 从此直接返回，这条连接的入站不再受限。
// 用一个"门"把 IO 线程卡在第 9 帧之后，让 2 在 3 之前确定发生。之后主线程不再消费，客户端继续灌：
// 在途必须被钉住（再次暂停），而不是无界增长。
class Gate final : public IConnectionHandler
{
public:
    Gate(IConnectionHandler& inner, int block_at)
        : inner_(inner)
        , block_at_(block_at)
    {
    }
    void OnConnected(IConnection& c) noexcept override { inner_.OnConnected(c); }
    void OnMessage(IConnection& c, std::span<const std::byte> payload) noexcept override
    {
        inner_.OnMessage(c, payload);
        if (seen_.fetch_add(1) + 1 == block_at_)
        {
            blocked_ = true;
            while (!release_.load())
                std::this_thread::sleep_for(100us);
        }
    }
    void OnClosed(IConnection& c, CloseReason r, int e) noexcept override { inner_.OnClosed(c, r, e); }

    bool blocked() const { return blocked_.load(); }
    void Release() { release_ = true; }

private:
    IConnectionHandler& inner_;
    const int block_at_;
    std::atomic<int> seen_{0};
    std::atomic<bool> blocked_{false};
    std::atomic<bool> release_{false};
};

void TestStaleResumeDoesNotDisableBackpressure()
{
    const LeakCheck leak;
    constexpr std::size_t kFrame = 100;
    App app("exec-test");
    MainLoopExecutor executor(app, Limits(8, 1ull << 30, 1ull << 30));
    Recorder handler;
    Gate gate(executor.Wrap(handler), /*block_at=*/9);
    LengthPrefixCodecFactory factory(kMaxFrame);
    EventLoopGroup group({1, 8});
    StreamServer server(group, *Endpoint::Tcp("127.0.0.1", 0), BigWater(), gate, factory);
    CHECK(group.Start() == 0);
    CHECK(server.Start() == 0);

    TestClient c(server.endpoint());
    std::vector<std::byte> batch; // 40 帧拼成一次 send：服务端一次 read 全部读进来
    for (int i = 0; i < 40; ++i)
    {
        const auto fr = TestClient::Frame(std::string(kFrame, 'a'));
        batch.insert(batch.end(), fr.begin(), fr.end());
    }
    c.SendAll(batch);

    CHECK(WaitUntil([&] { return gate.blocked(); })); // IO 线程停在第 9 帧之后，已经暂停
    CHECK(WaitUntil([&] { return group.GetStats().paused_connections == 1; }));
    const auto drain_until = [&](std::size_t messages)
    {
        const auto deadline = std::chrono::steady_clock::now() + kWait;
        while (handler.Count('M') < messages)
        {
            CHECK(std::chrono::steady_clock::now() < deadline);
            if (!executor.Drain(256))
                std::this_thread::sleep_for(200us);
        }
    };
    drain_until(9); // 主线程消费光：标志清掉，ResumeRead 已投递（IO 线程被卡着，执行不了）
    gate.Release(); // IO 线程继续分发剩下的 31 帧，在途又超过高水位；之后才执行那个过期的 ResumeRead

    // 之后主线程不再消费，客户端继续灌 2MB：在途必须被钉住，连接必须再次物理暂停
    std::thread flood(
        [&]
        {
            for (int i = 0; i < 20000; ++i)
                c.SendFrame(std::string(kFrame, 'b'));
        });
    CHECK(WaitUntil([&] { return group.GetStats().paused_connections == 1; }));
    std::this_thread::sleep_for(200ms);
    CHECK(group.GetStats().paused_connections == 1);
    // 一次 read ≤ 64KB（≈ 600 帧），加上刚才那批剩下的 31 帧：远小于 2MB
    CHECK(executor.GetStats().inflight_bytes <= 100 * 1024);
    drain_until(40 + 20000); // 消费之后恢复，flood 才能发完
    flood.join();
}

// ---- 析构 ----

// 未消费的事件随 executor 销毁，不回调业务；连接状态（shared_ptr）不泄漏（asan 下检查）。
void TestDestroyWithPendingEvents()
{
    const LeakCheck leak;
    auto handler = std::make_unique<Recorder>();
    {
        App app("exec-test");
        MainLoopExecutor executor(app);
        LengthPrefixCodecFactory factory(kMaxFrame);
        EventLoopGroup group({2, 64});
        StreamServer server(group, *Endpoint::Tcp("127.0.0.1", 0), BigWater(), executor.Wrap(*handler), factory);
        CHECK(group.Start() == 0);
        CHECK(server.Start() == 0);
        TestClient c(server.endpoint());
        for (int i = 0; i < 50; ++i)
            c.SendFrame("pending");
        CHECK(WaitUntil([&] { return executor.GetStats().queued_events >= 51; }));
        group.Shutdown(500ms); // 连接被关闭，Closed 也入了队，但谁也没 Drain
    }
    CHECK(handler->events.empty());
}

} // namespace

int main()
{
    TestEventsArriveInOrderOnMainLoop();
    TestDrainReturnValueAndLimit();
    TestClosedNeverOvertakes();
    TestManyConnectionsAcrossQueuesKeepPerConnectionOrder();
    TestPerConnectionBackpressure();
    TestGlobalBudgetPausesAllAndResumesAll();
    TestHysteresisBetweenHighAndLowWater();
    TestStaleResumeDoesNotDisableBackpressure();
    TestBackpressureStress();
    TestDestroyWithPendingEvents();
    std::puts("main_loop_executor_test: ok");
    return 0;
}
