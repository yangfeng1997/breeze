// rudp 端到端：真实的 RudpServer 与 RudpClient 经回环 UDP（以及可丢包 / 复制 / 乱序 / 黑洞的代理）通信。
// 消息边界与顺序、大消息、超限、双向、多客户端、多线程发送、恶劣网络、背压与慢消费者、优雅 / 强制关闭、空闲、
// 重来、服务端重启后的快速失败、停机、与 TCP 共用一个 group、发送延迟（flush_on_send）、积压计数。
// 无测试框架依赖：失败即 abort；每个用例都带超时，卡死也会 abort。

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "event_loop_group.h"
#include "group_access.h"
#include "net_test_util.h"
#include "rudp_client.h"
#include "rudp_server.h"
#include "rudp_test_util.h"
#include "tcp_server.h"

using namespace breeze;
using namespace breeze::testing;

// 每个用例开始前打一行名字：卡住 / 失败时从 ctest 日志一眼看出是哪个。
// 环境变量 RUDP_TEST_ONLY=<名字片段> 只跑名字里带它的用例（反复复现某个偶发失败时用）。
#define RUN(test)                                                                                                      \
    do                                                                                                                 \
    {                                                                                                                  \
        const char* only = std::getenv("RUDP_TEST_ONLY");                                                              \
        if (only == nullptr || std::strstr(#test, only) != nullptr)                                                    \
        {                                                                                                              \
            std::fprintf(stderr, "[ RUN ] %s\n", #test);                                                               \
            test();                                                                                                    \
        }                                                                                                              \
    } while (0)

namespace
{

struct Env
{
    EventLoopGroup group;
    EventRecorder srv; // 服务端看到的事件
    std::unique_ptr<RudpServer> server;

    explicit Env(RudpServerOptions so = {}, EventLoopGroupOptions go = EventLoopGroupOptions{2, 1024})
        : group(go)
    {
        CHECK(group.Start() == 0);
        server = std::make_unique<RudpServer>(group, RudpLoopback(), std::move(so), srv);
        CHECK(server->Start() == 0);
    }
    ~Env()
    {
        server.reset();
        group.Shutdown(5s);
    }

    // 新建一个客户端（自动向服务端要一张票据）。via 给了就连那个地址（比如代理），否则直连服务端。
    std::unique_ptr<RudpClient> Client(EventRecorder& rec, RudpClientOptions o = {}, const Endpoint* via = nullptr)
    {
        const auto t = server->IssueTicket(30s);
        CHECK(t.has_value());
        o.ticket = *t;
        return std::make_unique<RudpClient>(
            group, *RudpEndpoint::From(via != nullptr ? *via : server->endpoint()), std::move(o), rec);
    }

    // 建一个客户端并连上，返回时两端都已 OnConnected。
    std::unique_ptr<RudpClient> Connected(EventRecorder& rec, RudpClientOptions o = {}, const Endpoint* via = nullptr)
    {
        auto c = Client(rec, std::move(o), via);
        CHECK(c->Start() == 0);
        rec.WaitConnected(0);
        return c;
    }

    NetStats stats() const { return group.GetStats(); }
};

void EchoBack(EventRecorder& r)
{
    r.on_message = [](Connection& c, std::string_view sv) { Echo(c, sv); };
}

std::string Numbered(int i, std::size_t pad = 0)
{
    std::string s = "m" + std::to_string(i);
    s.resize(std::max(s.size(), pad), '.');
    return s;
}

// ---- 基本收发 ----

// 回显：消息边界不丢（空消息、各种长度），顺序不变；握手后两端都有对端信息。
void TestEchoBoundariesAndOrder()
{
    Env env;
    EchoBack(env.srv);
    EventRecorder cli;
    RudpClientOptions co;
    co.connection.high_water_mark = 8 * 1024 * 1024; // 测试线程一口气塞 ~450KB：句柄侧判水位、不能 flush
    auto c = env.Connected(cli, std::move(co));
    std::vector<std::string> want;
    for (int i = 0; i < 300; ++i)
        want.push_back(i % 7 == 0 ? std::string() : Numbered(i, static_cast<std::size_t>(i * 11 % 3000)));
    for (const auto& m : want)
        CHECK(c->Send(Buf(m)) == SendResult::Ok);
    CHECK(cli.WaitMessages(want.size(), 30s));
    CHECK(cli.payloads() == want);
    cli.CheckInvariants();
    env.srv.CheckInvariants();
}

// 双方的对端信息：服务端看到客户端的 udp 地址，客户端看到服务端的；没有 credentials。
void TestPeerInfo()
{
    std::mutex mu; // 回调引用的状态放在 env 之前：env 析构时回调可能还在跑
    std::optional<Endpoint> server_saw, client_saw;
    bool creds = false;
    Env env;
    env.srv.on_connected = [&](Connection& c)
    {
        const std::lock_guard lk(mu);
        server_saw = c.peer().endpoint;
        creds = creds || c.peer().credentials.has_value();
    };
    EventRecorder cli;
    cli.on_connected = [&](Connection& c)
    {
        const std::lock_guard lk(mu);
        client_saw = c.peer().endpoint;
        creds = creds || c.peer().credentials.has_value();
    };
    auto c = env.Connected(cli);
    env.srv.WaitConnected(0);
    const std::lock_guard lk(mu);
    CHECK(client_saw.has_value() && *client_saw == env.server->endpoint());
    CHECK(server_saw.has_value() && server_saw->kind() == Endpoint::Kind::Udp && server_saw->address() == "127.0.0.1");
    CHECK(server_saw->port() != 0 && server_saw->port() != env.server->endpoint().port());
    CHECK(!creds);
}

// 大消息：分成多段、收齐才交付一次；最大的一条（max_message_size）也能回显。
void TestLargeMessages()
{
    RudpServerOptions so;
    so.rudp.max_message_size = 120000;
    Env env(std::move(so));
    EchoBack(env.srv);
    EventRecorder cli;
    RudpClientOptions co;
    co.rudp.max_message_size = 120000;
    auto c = env.Connected(cli, std::move(co));
    std::string big(120000, '\0');
    for (std::size_t i = 0; i < big.size(); ++i)
        big[i] = static_cast<char>('A' + i % 23);
    const std::string mid(10000, 'x');
    CHECK(c->Send(Buf(mid)) == SendResult::Ok);
    CHECK(c->Send(Buf(big)) == SendResult::Ok);
    CHECK(c->Send(Buf("tail")) == SendResult::Ok);
    CHECK(cli.WaitMessages(3, 30s));
    CHECK(cli.payloads() == (std::vector<std::string>{mid, big, "tail"}));
}

// 超过 max_message_size 的发送：和 TCP 的 codec 超限一样关闭连接（ProtocolError），计数。
void TestOversizeSendClosesConnection()
{
    Env env;
    EventRecorder cli;
    auto c = env.Connected(cli);
    CHECK(c->Send(Buf(std::string(64 * 1024, 'x'))) == SendResult::Ok);     // 恰好上限
    CHECK(c->Send(Buf(std::string(64 * 1024 + 1, 'x'))) == SendResult::Ok); // 投递成功，在 IO 线程上发现超限
    const auto closed = cli.WaitClosed(0);
    CHECK(closed.reason == CloseReason::ProtocolError);
    CHECK(env.srv.WaitClosed(0).reason == CloseReason::PeerClosed); // 对端收到 Close
    CHECK(env.stats().send_closed_oversize == 1);
}

// 双向同时发：服务端在 OnConnected 里就开始发（Accept 先于它们到），客户端同时发；两边都收齐、按序。
void TestBidirectionalBulk()
{
    Env env;
    constexpr int kN = 500;
    env.srv.on_connected = [](Connection& c)
    {
        for (int i = 0; i < kN; ++i)
        {
            const std::string m = Numbered(i);
            CHECK(c.Send(std::as_bytes(std::span(m))) == SendResult::Ok);
        }
    };
    EventRecorder cli;
    auto c = env.Client(cli);
    CHECK(c->Start() == 0);
    cli.WaitConnected(0);
    for (int i = 0; i < kN; ++i)
        CHECK(c->Send(Buf(Numbered(i))) == SendResult::Ok);
    CHECK(cli.WaitMessages(kN, 30s) && env.srv.WaitMessages(kN, 30s));
    for (int i = 0; i < kN; ++i)
    {
        CHECK(cli.payloads()[static_cast<std::size_t>(i)] == Numbered(i));
        CHECK(env.srv.payloads()[static_cast<std::size_t>(i)] == Numbered(i));
    }
}

// 多个客户端并发：按来源分得开，各自的回显只回给各自，顺序不乱。
void TestManyClients()
{
    Env env(RudpServerOptions{}, EventLoopGroupOptions{3, 512});
    EchoBack(env.srv);
    constexpr int kClients = 40, kMsgs = 50;
    std::vector<std::unique_ptr<EventRecorder>> recs;
    std::vector<std::unique_ptr<RudpClient>> clients;
    for (int i = 0; i < kClients; ++i)
    {
        recs.push_back(std::make_unique<EventRecorder>());
        clients.push_back(env.Client(*recs.back()));
        CHECK(clients.back()->Start() == 0);
    }
    for (int i = 0; i < kClients; ++i)
        recs[static_cast<std::size_t>(i)]->WaitConnected(0);
    for (int m = 0; m < kMsgs; ++m)
        for (int i = 0; i < kClients; ++i)
            CHECK(clients[static_cast<std::size_t>(i)]->Send(Buf("c" + std::to_string(i) + "-" + std::to_string(m))) ==
                  SendResult::Ok);
    for (int i = 0; i < kClients; ++i)
    {
        auto& r = *recs[static_cast<std::size_t>(i)];
        CHECK(r.WaitMessages(kMsgs, 30s));
        const auto got = r.payloads();
        CHECK(got.size() == kMsgs);
        for (int m = 0; m < kMsgs; ++m)
            CHECK(got[static_cast<std::size_t>(m)] == "c" + std::to_string(i) + "-" + std::to_string(m));
    }
    CHECK(env.stats().accepted == kClients && env.stats().connected == kClients);
}

// 多个线程同时经 RudpClient::Send（句柄）发：每个线程自己的消息保持顺序，总数不少。
void TestSendFromManyThreads()
{
    Env env;
    EventRecorder cli;
    auto c = env.Connected(cli);
    constexpr int kThreads = 4, kPer = 400;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back(
            [&, t]
            {
                for (int i = 0; i < kPer; ++i)
                    CHECK(c->Send(Buf("t" + std::to_string(t) + ":" + std::to_string(i))) == SendResult::Ok);
            });
    }
    for (auto& th : threads)
        th.join();
    CHECK(env.srv.WaitMessages(kThreads * kPer, 30s));
    std::vector<int> next(kThreads, 0);
    for (const auto& m : env.srv.payloads())
    {
        const auto colon = m.find(':');
        const int t = std::stoi(m.substr(1, colon - 1));
        const int i = std::stoi(m.substr(colon + 1));
        CHECK(i == next[static_cast<std::size_t>(t)]++);
    }
}

// ---- 恶劣网络 ----

// 丢包 + 复制 + 乱序下的回显：握手（Hello 重传）与数据（KCP 重传）都扛得住，消息不丢、不重、不乱序。
// 两端的水位都放宽：测试线程一口气塞进 n 条，回显的队列在 ack 被丢掉时会涨过默认的 256KB——那是"突发超过窗口 + 水位"的
// 慢消费者判定，不是本用例要测的。
void LossyRoundTrip(int loss_pct, int n)
{
    RudpServerOptions so;
    so.connection.high_water_mark = 8 * 1024 * 1024;
    Env env(std::move(so));
    EchoBack(env.srv);
    UdpProxy proxy(env.server->endpoint());
    proxy.SetRules({.loss_up_pct = loss_pct, .loss_down_pct = loss_pct, .dup_pct = 3, .reorder_pct = 8});
    EventRecorder cli;
    RudpClientOptions co;
    co.connection.high_water_mark = 8 * 1024 * 1024;
    auto c = env.Client(cli, std::move(co), &proxy.endpoint());
    CHECK(c->Start() == 0);
    cli.WaitConnected(0, 30s);
    for (int i = 0; i < n; ++i)
        CHECK(c->Send(Buf(Numbered(i, static_cast<std::size_t>(i % 5) * 300))) == SendResult::Ok);
    if (!cli.WaitMessages(static_cast<std::size_t>(n), 60s))
    {
        // 活性问题要看现场：两端是否被关、为什么、代理转发 / 丢弃了多少
        const auto reason = [](EventRecorder& r)
        { return r.closed_count() == 0 ? "-" : ToString(r.WaitClosed(0, 1ms).reason); };
        std::fprintf(
            stderr,
            "LossyRoundTrip(%d%%, %d): 回显 %zu 条，客户端关闭 %zu 次（%s），服务端关闭 %zu 次（%s，收到 %zu 条），"
            "代理转发 %llu 丢弃 %llu\n",
            loss_pct,
            n,
            cli.message_count(),
            cli.closed_count(),
            reason(cli),
            env.srv.closed_count(),
            reason(env.srv),
            env.srv.message_count(),
            static_cast<unsigned long long>(proxy.forwarded()),
            static_cast<unsigned long long>(proxy.dropped()));
        CHECK(false);
    }
    const auto got = cli.payloads();
    CHECK(got.size() == static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        CHECK(got[static_cast<std::size_t>(i)] == Numbered(i, static_cast<std::size_t>(i % 5) * 300));
    CHECK(proxy.dropped() > static_cast<std::uint64_t>(n / 20)); // 确实丢了不少
    CHECK(cli.closed_count() == 0 && env.srv.closed_count() == 0);
}

void TestLossyNetwork() { LossyRoundTrip(15, 800); }
void TestVeryLossyNetwork() { LossyRoundTrip(35, 300); }

// 黑洞：网络中断一阵（短于链路判死），中断期间发的消息在恢复后送达，连接不断。
void TestBlackholeRecovery()
{
    Env env;
    UdpProxy proxy(env.server->endpoint());
    EventRecorder cli;
    auto c = env.Connected(cli, RudpClientOptions{}, &proxy.endpoint());
    proxy.SetRules({.blackhole = true});
    for (int i = 0; i < 50; ++i)
        CHECK(c->Send(Buf(Numbered(i))) == SendResult::Ok);
    std::this_thread::sleep_for(1500ms);
    CHECK(env.srv.message_count() == 0);
    proxy.SetRules({});
    CHECK(env.srv.WaitMessages(50, 30s));
    CHECK(env.srv.closed_count() == 0 && cli.closed_count() == 0);
    for (int i = 0; i < 50; ++i)
        CHECK(env.srv.payloads()[static_cast<std::size_t>(i)] == Numbered(i));
}

// 链路判死：网络一直不通，发出去的消息重传 dead_link 次仍没确认，客户端以 IoError（ETIMEDOUT）关闭；
// 服务端没有未确认的数据，靠 idle_timeout 关闭。
void TestDeadLinkClosesBothSides()
{
    RudpServerOptions so;
    so.connection.idle_timeout = 1000ms;
    Env env(std::move(so));
    UdpProxy proxy(env.server->endpoint());
    RudpClientOptions co;
    co.rudp.dead_link = 5;
    EventRecorder cli;
    auto c = env.Connected(cli, std::move(co), &proxy.endpoint());
    proxy.SetRules({.blackhole = true});
    CHECK(c->Send(Buf("into the void")) == SendResult::Ok);
    const auto closed = cli.WaitClosed(0, 20s);
    CHECK(closed.reason == CloseReason::IoError && closed.err == ETIMEDOUT);
    CHECK(env.srv.WaitClosed(0, 20s).reason == CloseReason::IdleTimeout);
}

// ---- 背压与慢消费者 ----

// 接收方暂停读：不再向 handler 交付，消息留在 KCP 接收队列里，队列占满窗口后发送方被自然拖慢；
// 恢复后一条不丢、按序补交付。
void TestPauseBackpressureThenResume()
{
    std::atomic<int> got{0};
    Env env;
    env.srv.on_message = [&](Connection& c, std::string_view)
    {
        if (got.fetch_add(1) + 1 == 5)
            c.PauseRead();
    };
    EventRecorder cli;
    auto c = env.Connected(cli);
    constexpr int kN = 2000;
    for (int i = 0; i < kN; ++i)
        CHECK(c->Send(Buf(Numbered(i))) == SendResult::Ok);
    CHECK(WaitUntil([&] { return got.load() >= 5; }));
    std::this_thread::sleep_for(500ms);
    CHECK(got.load() == 5); // 暂停之后一条都没再交付
    CHECK(env.stats().paused_connections == 1);
    const ConnectionHandle h = env.srv.WaitConnected(0);
    h.ResumeRead();
    // 恢复之后很快就补交付（下一次 Tick，不是等对端 7 秒之后的窗口探测）
    CHECK(WaitUntil([&] { return got.load() > 5; }, 1s));
    CHECK(env.srv.WaitMessages(kN, 30s));
    CHECK(env.stats().paused_connections == 0);
    const auto all = env.srv.payloads();
    for (int i = 0; i < kN; ++i)
        CHECK(all[static_cast<std::size_t>(i)] == Numbered(i));
    CHECK(env.srv.closed_count() == 0);
}

// 对端一直不消费：发送方的积压（排队 + 在途未确认）越过水位，Send 返回 Closed，连接以 SlowConsumer 关闭。
void TestSlowConsumerHighWater()
{
    Env env;
    env.srv.on_message = [](Connection& c, std::string_view) { c.PauseRead(); }; // 收到第一条就不再读
    EventRecorder cli;
    RudpClientOptions co;
    co.connection.high_water_mark = 64 * 1024;
    auto c = env.Connected(cli, std::move(co));
    const std::string chunk(4096, 'z');
    bool closed = false;
    for (int i = 0; i < 400 && !closed; ++i)
    {
        closed = c->Send(Buf(chunk)) == SendResult::Closed;
        if (!closed && i % 20 == 19)
            std::this_thread::sleep_for(20ms); // 让 KCP 有机会把队列往窗口里移，直到对端窗口占满
    }
    CHECK(closed); // 对端不再读：窗口占满之后队列涨过水位，Send 返回 Closed
    CHECK(cli.WaitClosed(0, 10s).reason == CloseReason::SlowConsumer);
    CHECK(env.stats().send_closed_slow_consumer >= 1);
}

// 暂停读的会话不计空闲：服务端开了 idle_timeout，暂停之后对端一声不吭，也不会被空闲检测关掉；
// 恢复之后重新计时，对端仍不吭声才以 IdleTimeout 关闭。
void TestPausedSessionIsNotIdleClosed()
{
    RudpServerOptions so;
    so.connection.idle_timeout = 600ms;
    Env env(std::move(so));
    env.srv.on_message = [](Connection& c, std::string_view) { c.PauseRead(); };
    EventRecorder cli;
    RudpClientOptions co;
    co.ping_interval = 0ms; // 不发心跳：对端静默
    auto c = env.Connected(cli, std::move(co));
    CHECK(c->Send(Buf("pause me")) == SendResult::Ok);
    CHECK(env.srv.WaitMessages(1));
    std::this_thread::sleep_for(2500ms); // 远超 idle_timeout
    CHECK(env.srv.closed_count() == 0);
    env.srv.WaitConnected(0).ResumeRead();
    std::this_thread::sleep_for(250ms);
    CHECK(env.srv.closed_count() == 0); // 恢复时重新计时，不是一恢复就判超时
    CHECK(env.srv.WaitClosed(0, 8s).reason == CloseReason::IdleTimeout);
}

// ---- 关闭 ----

// 优雅关闭：已排队的消息被对端全部确认之后才结束；发起方 LocalClose，对端 PeerClosed。
void TestGracefulCloseDeliversEverything()
{
    Env env;
    EventRecorder cli;
    auto c = env.Connected(cli);
    constexpr int kN = 500;
    for (int i = 0; i < kN; ++i)
        CHECK(c->Send(Buf(Numbered(i, 200))) == SendResult::Ok);
    const auto t0 = std::chrono::steady_clock::now();
    c->handle().CloseGracefully(10s);
    (void)c->Send(Buf("late")); // 排在 CloseGracefully 之后：到 IO 线程时连接已在优雅关闭，被丢弃
    CHECK(env.srv.WaitMessages(kN, 30s));
    CHECK(env.srv.WaitClosed(0, 10s).reason == CloseReason::PeerClosed);
    CHECK(cli.WaitClosed(0, 10s).reason == CloseReason::LocalClose);
    CHECK(std::chrono::steady_clock::now() - t0 < 5s);
    for (int i = 0; i < kN; ++i)
        CHECK(env.srv.payloads()[static_cast<std::size_t>(i)] == Numbered(i, 200));
    CHECK(env.srv.message_count() == kN); // "late" 没有进来
}

// 优雅关闭的期限：对端一直不确认，到期强关，原因仍是 LocalClose。
void TestGracefulCloseTimesOut()
{
    Env env;
    UdpProxy proxy(env.server->endpoint());
    EventRecorder cli;
    auto c = env.Connected(cli, RudpClientOptions{}, &proxy.endpoint());
    proxy.SetRules({.blackhole = true});
    CHECK(c->Send(Buf("never acked")) == SendResult::Ok);
    std::this_thread::sleep_for(100ms);
    const auto t0 = std::chrono::steady_clock::now();
    c->handle().CloseGracefully(600ms);
    const auto closed = cli.WaitClosed(0, 10s);
    const auto took = std::chrono::steady_clock::now() - t0;
    CHECK(closed.reason == CloseReason::LocalClose);
    CHECK(took >= 500ms && took < 3s);
}

// 强制关闭：对端很快以 PeerClosed 结束；服务端主动关，客户端同样。
void TestHardClose()
{
    Env env;
    EventRecorder cli;
    auto c = env.Connected(cli);
    const ConnectionHandle sh = env.srv.WaitConnected(0);
    const auto t0 = std::chrono::steady_clock::now();
    c->handle().Close();
    CHECK(cli.WaitClosed(0).reason == CloseReason::LocalClose);
    CHECK(env.srv.WaitClosed(0).reason == CloseReason::PeerClosed);
    CHECK(std::chrono::steady_clock::now() - t0 < 1s);
    CHECK(c->Send(Buf("x")) == SendResult::Closed);
    CHECK(sh.Send(Buf("x")) == SendResult::Closed);

    EventRecorder cli2;
    auto c2 = env.Connected(cli2);
    const ConnectionHandle sh2 = env.srv.WaitConnected(1);
    sh2.Close();
    CHECK(env.srv.WaitClosed(1).reason == CloseReason::LocalClose);
    CHECK(cli2.WaitClosed(0).reason == CloseReason::PeerClosed);
}

// 关闭之后的发送：句柄过期，得到 Closed，计入 send_closed_stale。
void TestSendAfterClose()
{
    Env env;
    EventRecorder cli;
    auto c = env.Connected(cli);
    const ConnectionHandle h = c->handle();
    h.Close();
    cli.WaitClosed(0);
    const auto before = env.stats().send_closed_stale;
    CHECK(h.Send(Buf("x")) == SendResult::Closed);
    CHECK(env.stats().send_closed_stale == before + 1); // 句柄过期：同步判出，同步计数
    CHECK(c->Send(Buf("x")) == SendResult::Closed);     // 客户端对象的句柄已清空：直接 Closed，不再计数
    CHECK(env.stats().send_closed_stale == before + 1);
}

// ---- 空闲 ----

// 服务端开了 idle_timeout：定期发心跳的客户端不会被关；不发心跳的被以 IdleTimeout 关闭，客户端收到 Close。
void TestIdleTimeoutAndHeartbeat()
{
    RudpServerOptions so;
    so.connection.idle_timeout = 700ms;
    Env env(std::move(so));
    EventRecorder alive_rec, quiet_rec;
    RudpClientOptions alive_o;
    alive_o.ping_interval = 100ms;
    RudpClientOptions quiet_o;
    quiet_o.ping_interval = 0ms;
    auto alive = env.Connected(alive_rec, std::move(alive_o));
    auto quiet = env.Connected(quiet_rec, std::move(quiet_o));
    const auto closed = quiet_rec.WaitClosed(0, 8s);
    CHECK(closed.reason == CloseReason::PeerClosed); // 服务端关它时发的 Close
    CHECK(env.srv.closed_count() == 1 && env.srv.WaitClosed(0).reason == CloseReason::IdleTimeout);
    std::this_thread::sleep_for(1500ms);
    CHECK(alive_rec.closed_count() == 0 && alive->handle().valid()); // 心跳的那个一直在
    CHECK(env.stats().closed[static_cast<std::size_t>(CloseReason::IdleTimeout)] == 1);
}

// ---- 重来与重启 ----

// 断线后用新票据重来：新连接可用，句柄是新的；旧票据不能再用。
void TestReconnectWithNewTicket()
{
    Env env;
    EchoBack(env.srv);
    EventRecorder cli;
    auto c = env.Client(cli);
    CHECK(c->Start() == 0);
    const ConnectionHandle h1 = cli.WaitConnected(0);
    const RudpTicket old = *env.server->IssueTicket(30s); // 另一张，留着
    env.srv.WaitConnected(0).Close();
    cli.WaitClosed(0);
    CHECK(c->handle() == ConnectionHandle{});

    c->SetTicket(old);
    CHECK(c->Start() == 0);
    const ConnectionHandle h2 = cli.WaitConnected(1);
    CHECK(h1 != h2);
    CHECK(c->Send(Buf("again")) == SendResult::Ok);
    CHECK(cli.WaitMessages(1));
    CHECK(cli.payloads()[0] == "again");
    CHECK(env.stats().accepted == 2 && env.stats().connected == 2);
    cli.CheckInvariants();
}

// 用过的票据不能再用：第二次 Start 带同一张票据，握手被拒（EACCES）。
void TestUsedTicketIsRejected()
{
    Env env;
    std::atomic<int> err{0};
    EventRecorder cli;
    RudpClientOptions co;
    co.on_connect_failed = [&](int e, bool) noexcept { err = e; };
    auto c = env.Client(cli, std::move(co));
    CHECK(c->Start() == 0);
    cli.WaitConnected(0);
    env.srv.WaitConnected(0).Close();
    cli.WaitClosed(0);
    CHECK(c->Start() == 0); // 还是那张票据
    CHECK(WaitUntil([&] { return err.load() != 0; }));
    CHECK(err.load() == EACCES);
    CHECK(env.stats().rejected_ticket == 1);
}

// 服务端重启（没来得及发 Close）：客户端的下一个包撞上新服务端，新服务端不认识这个来源，回 Close，客户端快速失败，
// 而不是干等心跳超时。
void TestServerRestartFailsFast()
{
    Env env;
    UdpProxy proxy(env.server->endpoint());
    const std::uint16_t port = env.server->endpoint().port();
    EventRecorder cli;
    RudpClientOptions co;
    co.ping_interval = 100ms;
    auto c = env.Connected(cli, std::move(co), &proxy.endpoint());
    proxy.SetRules({.blackhole = true}); // 旧服务端关闭时发的 Close 被吞掉，客户端不知道
    env.server.reset();
    EventRecorder dummy;
    env.server = std::make_unique<RudpServer>(env.group, RudpLoopback(port), RudpServerOptions{}, dummy);
    CHECK(env.server->Start() == 0);
    CHECK(cli.closed_count() == 0);
    proxy.SetRules({});
    const auto closed = cli.WaitClosed(0, 5s);
    CHECK(closed.reason == CloseReason::PeerClosed);
}

// ---- 停机与泄漏 ----

// group 停机时连接上仍有流量：优雅关闭，所有连接都回调 OnClosed，不死锁、不泄漏。
void TestShutdownWithLiveTraffic()
{
    LeakCheck leak;
    {
        EventLoopGroup group(EventLoopGroupOptions{2, 256});
        CHECK(group.Start() == 0);
        EventRecorder srv, cli;
        srv.on_message = [](Connection& c, std::string_view sv) { Echo(c, sv); };
        RudpServer server(group, RudpLoopback(), RudpServerOptions{}, srv);
        CHECK(server.Start() == 0);
        std::vector<std::unique_ptr<RudpClient>> clients;
        for (int i = 0; i < 8; ++i)
        {
            RudpClientOptions o;
            o.ticket = *server.IssueTicket(30s);
            clients.push_back(
                std::make_unique<RudpClient>(group, *RudpEndpoint::From(server.endpoint()), std::move(o), cli));
            CHECK(clients.back()->Start() == 0);
        }
        for (int i = 0; i < 8; ++i)
            cli.WaitConnected(static_cast<std::size_t>(i));
        std::atomic<bool> stop{false};
        std::thread pump(
            [&]
            {
                while (!stop.load())
                {
                    for (auto& c : clients)
                        (void)c->Send(Buf("traffic"));
                    std::this_thread::sleep_for(2ms);
                }
            });
        std::this_thread::sleep_for(200ms);
        group.Shutdown(3s);
        stop = true;
        pump.join();
        CHECK(cli.closed_count() == 8 && srv.closed_count() == 8);
        cli.CheckInvariants();
        srv.CheckInvariants();
    }
}

// 会话反复建立与关闭：几个线程各自快速地 连接 → 回显一条 → 以不同方式结束（Stop / 优雅关闭 / 直接销毁），
// 同时还有一条长连接一直在回显。活跃表的增删、摘表、swap-remove 在这种乱序下不能崩（ASAN 下检查悬空指针），
// 长连接始终可用，最后服务端只剩长连接的会话。
void TestSessionChurn()
{
    RudpServerOptions so;
    so.connection.idle_timeout = 700ms; // 长连接靠心跳活过它；万一哪个短连接没被正常关掉，它也会兜底清走
    Env env(std::move(so), EventLoopGroupOptions{3, 1024});
    EchoBack(env.srv);

    EventRecorder keep_rec;
    RudpClientOptions keep_o;
    keep_o.ping_interval = 100ms; // 长连接靠心跳活过服务端的空闲超时
    auto keeper = env.Connected(keep_rec, std::move(keep_o));

    constexpr int kThreads = 4, kPer = 25;
    std::atomic<int> done{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back(
            [&, t]
            {
                for (int i = 0; i < kPer; ++i)
                {
                    EventRecorder rec;
                    RudpClientOptions o;
                    auto c = env.Client(rec, std::move(o));
                    CHECK(c->Start() == 0);
                    rec.WaitConnected(0);
                    const std::string m = "churn-" + std::to_string(t) + "-" + std::to_string(i);
                    CHECK(c->Send(Buf(m)) == SendResult::Ok);
                    CHECK(rec.WaitMessages(1));
                    CHECK(rec.payloads()[0] == m);
                    switch ((t + i) % 4)
                    {
                        case 0:
                            c->Stop();
                            break;
                        case 1:
                            c->handle().CloseGracefully(2s);
                            rec.WaitClosed(0, 5s);
                            break;
                        case 2:
                            break; // 直接销毁：析构里同步关闭
                        case 3:
                            c->Stop();
                            std::this_thread::sleep_for(1ms);
                            break;
                    }
                    c.reset();
                    done.fetch_add(1);
                }
            });
    }
    // 长连接在整个过程中一直能回显
    int round = 0;
    while (done.load() < kThreads * kPer)
    {
        const std::string m = "keep-" + std::to_string(round++);
        CHECK(keeper->Send(Buf(m)) == SendResult::Ok);
        CHECK(keep_rec.WaitMessages(static_cast<std::size_t>(round), 20s));
        std::this_thread::sleep_for(20ms);
    }
    for (auto& th : threads)
        th.join();
    CHECK(keep_rec.closed_count() == 0 && keeper->handle().valid());
    // 只剩长连接的两端（服务端会话 + 客户端连接）
    CHECK(WaitUntil([&] { return env.stats().connections == 2; }, 20s));
    CHECK(env.stats().accepted == kThreads * kPer + 1);
}

// 与 TCP 共用一个 group：同一批 loop 上同时有 TCP 与 rudp 连接，各自收发不串。
void TestSharesGroupWithTcp()
{
    EventLoopGroup group(EventLoopGroupOptions{2, 256});
    CHECK(group.Start() == 0);
    EventRecorder tcp_srv, rudp_srv, rudp_cli;
    tcp_srv.on_message = [](Connection& c, std::string_view sv) { Echo(c, sv); };
    rudp_srv.on_message = [](Connection& c, std::string_view sv) { Echo(c, "R:" + std::string(sv)); };
    LengthPrefixCodecFactory factory(1 << 20);
    TcpServer tcp(group, *TcpEndpoint::Make("127.0.0.1", 0), TcpServerOptions{}, tcp_srv, factory);
    CHECK(tcp.Start() == 0);
    RudpServer rudp(group, RudpLoopback(), RudpServerOptions{}, rudp_srv);
    CHECK(rudp.Start() == 0);

    TestClient tc(tcp.endpoint());
    RudpClientOptions o;
    o.ticket = *rudp.IssueTicket(30s);
    RudpClient rc(group, *RudpEndpoint::From(rudp.endpoint()), std::move(o), rudp_cli);
    CHECK(rc.Start() == 0);
    rudp_cli.WaitConnected(0);

    tc.SendFrame("over-tcp");
    CHECK(tc.RecvFrame() == "over-tcp");
    CHECK(rc.Send(Buf("over-rudp")) == SendResult::Ok);
    CHECK(rudp_cli.WaitMessages(1));
    CHECK(rudp_cli.payloads()[0] == "R:over-rudp");
    CHECK(tcp_srv.message_count() == 1 && rudp_srv.message_count() == 1);
    CHECK(group.GetStats().connections == 3); // TCP 服务端 1 + rudp 服务端会话 1 + rudp 客户端连接 1
    group.Shutdown(2s);
}

// 连接上下文：和 TCP 一样随连接存在、随连接销毁。
void TestConnectionContext()
{
    struct Note final : ConnectionContext
    {
        explicit Note(int v)
            : value(v)
        {
        }
        int value;
    };
    std::atomic<int> seen{0};
    Env env;
    env.srv.on_connected = [](Connection& c) { c.SetContext(std::make_unique<Note>(42)); };
    env.srv.on_message = [&](Connection& c, std::string_view) { seen = c.GetContext<Note>()->value; };
    EventRecorder cli;
    auto c = env.Connected(cli);
    CHECK(c->Send(Buf("x")) == SendResult::Ok);
    CHECK(WaitUntil([&] { return seen.load() == 42; }));
}

// ---- 性能相关的行为 ----

// flush_on_send：开了，消息在 Send 时就发出去（单向延迟远小于 KCP 的 interval）；没开，要等下一次定时 Update。
// interval 取最大的 100ms，差别才明显；用 20 个不同相位的发送，没开时全都 < 40ms 的概率约 1e-8。
void TestFlushOnSendLatency()
{
    const auto measure = [](bool flush)
    {
        std::mutex mu; // 回调引用的状态放在 env 之前：env 析构时回调可能还在跑
        std::vector<std::chrono::steady_clock::duration> lat;
        RudpServerOptions so;
        so.rudp.interval_ms = 100;
        Env env(std::move(so));
        env.srv.on_message = [&](Connection&, std::string_view sv)
        {
            const auto sent =
                std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(std::stoll(std::string(sv))));
            const std::lock_guard lk(mu);
            lat.push_back(std::chrono::steady_clock::now() - sent);
        };
        EventRecorder cli;
        RudpClientOptions co;
        co.rudp.interval_ms = 100;
        co.rudp.flush_on_send = flush;
        auto c = env.Connected(cli, std::move(co));
        for (int i = 0; i < 20; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(37));
            const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            CHECK(c->Send(Buf(std::to_string(now))) == SendResult::Ok);
        }
        CHECK(env.srv.WaitMessages(20, 10s));
        const std::lock_guard lk(mu);
        auto worst = std::chrono::steady_clock::duration::zero();
        for (const auto d : lat)
            worst = std::max(worst, d);
        return std::chrono::duration_cast<std::chrono::milliseconds>(worst);
    };
    const auto with_flush = measure(true);
    const auto without = measure(false);
    CHECK(with_flush < 40ms);
    CHECK(without >= 40ms);
}

// 积压计数：消息排进 KCP 的发送队列就计入槽的 pending_bytes，整条进了发送窗口才减掉（已发出等确认的不算）。
// 窗口只有 4 段、网络又不通：头 4 条进窗口，其余 6 条一直在队列里，恢复后随确认逐条进窗口，最终回到 0。
void TestPendingBytesAccounting()
{
    Env env;
    UdpProxy proxy(env.server->endpoint());
    EventRecorder cli;
    RudpClientOptions co;
    co.rudp.send_window = 4;
    auto c = env.Connected(cli, std::move(co), &proxy.endpoint());
    const ConnectionHandle h = c->handle();
    EventLoop& loop = GroupAccess::LoopAt(env.group, h.loop());
    proxy.SetRules({.blackhole = true}); // 发出去没人确认，窗口不会再空出来
    for (int i = 0; i < 10; ++i)
        CHECK(c->Send(Buf(std::string(1000, 'p'))) == SendResult::Ok);
    CHECK(WaitUntil([&] { return loop.table().PendingBytes(h.slot()) == 6000; }));
    std::this_thread::sleep_for(300ms);
    CHECK(loop.table().PendingBytes(h.slot()) == 6000); // 窗口没空出来，队列里的不动
    proxy.SetRules({});
    CHECK(WaitUntil([&] { return loop.table().PendingBytes(h.slot()) == 0; }, 20s));
    CHECK(env.srv.WaitMessages(10, 20s));
}

// 水位只算排队、不算在途，且突发能进窗口就不是慢消费者：服务端在 IO 线程里一次发 150 条 × 1000 字节（150KB，远大于
// 水位 32KB）。握手时对端的窗口按 KCP 的初始值 128 段算，所以 22KB 留在队列里，没越过水位；客户端照常收齐，两边都不关。
void TestBurstWithinWindowIsNotSlowConsumer()
{
    RudpServerOptions so;
    so.connection.high_water_mark = 32 * 1024;
    Env env(std::move(so));
    env.srv.on_connected = [](Connection& c)
    {
        const std::string m(1000, 'b');
        for (int i = 0; i < 150; ++i)
            CHECK(c.Send(std::as_bytes(std::span(m))) == SendResult::Ok);
    };
    EventRecorder cli;
    auto c = env.Connected(cli);
    CHECK(cli.WaitMessages(150, 20s));
    CHECK(cli.closed_count() == 0 && env.srv.closed_count() == 0);
}

} // namespace

int main()
{
    RUN(TestEchoBoundariesAndOrder);
    RUN(TestPeerInfo);
    RUN(TestLargeMessages);
    RUN(TestOversizeSendClosesConnection);
    RUN(TestBidirectionalBulk);
    RUN(TestManyClients);
    RUN(TestSendFromManyThreads);
    RUN(TestLossyNetwork);
    RUN(TestVeryLossyNetwork);
    RUN(TestBlackholeRecovery);
    RUN(TestDeadLinkClosesBothSides);
    RUN(TestPauseBackpressureThenResume);
    RUN(TestSlowConsumerHighWater);
    RUN(TestPausedSessionIsNotIdleClosed);
    RUN(TestGracefulCloseDeliversEverything);
    RUN(TestGracefulCloseTimesOut);
    RUN(TestHardClose);
    RUN(TestSendAfterClose);
    RUN(TestIdleTimeoutAndHeartbeat);
    RUN(TestReconnectWithNewTicket);
    RUN(TestUsedTicketIsRejected);
    RUN(TestServerRestartFailsFast);
    RUN(TestShutdownWithLiveTraffic);
    RUN(TestSessionChurn);
    RUN(TestSharesGroupWithTcp);
    RUN(TestConnectionContext);
    RUN(TestFlushOnSendLatency);
    RUN(TestPendingBytesAccounting);
    RUN(TestBurstWithinWindowIsNotSlowConsumer);
    std::puts("rudp_test: ok");
    return 0;
}
