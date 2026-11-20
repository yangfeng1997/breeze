// WebSocket 端到端：真 socket、真 EventLoop。服务端是 StreamServer + WsServerCodecFactory；客户端有两种：
// WsRawClient（自己拼请求、自己编码帧的对照客户端，与被测代码无共用）和库自己的 StreamClient + WsClientCodecFactory。
//
// 覆盖单元测试（ws_codec_test，假宿主）测不到的：OnConnected 与 101 应答 / 首条 Send 的先后（状态必须先于 OnConnected 就绪）、
// 握手超时（慢速攻击）与握手失败统计、关闭码经 OnClosed 带给业务、优雅关闭的 Close(1001)、广播共享 payload、
// 客户端经 StreamClient 的收发 / 重连 / 握手被拒后的退避重试、ws 叠在字节流层上（wss 的叠加方式：层握手 + codec 握手同时存在，
// 客户端掩码走分层连接）。无测试框架依赖：失败即 abort。

#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "base/net/loop/event_loop_group.h"
#include "base/net/loop/group_access.h"
#include "base/net/stream/stream_client.h"
#include "base/net/stream/stream_server.h"
#include "base/net/testing/fake_layer_test_util.h"
#include "base/net/testing/net_test_util.h"
#include "base/net/testing/ws_test_util.h"
#include "base/net/ws/ws_codec.h"

using namespace breeze;
using namespace breeze::testing;

namespace
{

// ---- 夹具 ----

StreamServerOptions ServerOptions(std::chrono::milliseconds handshake_timeout, std::shared_ptr<FakeLayerFactory> layer)
{
    StreamServerOptions o;
    o.connection.handshake_timeout = handshake_timeout;
    o.tls = std::move(layer);
    return o;
}

struct WsServer
{
    explicit WsServer(WsServerOptions wo = {},
                      std::chrono::milliseconds handshake_timeout = 5s,
                      std::shared_ptr<FakeLayerFactory> layer = nullptr,
                      std::uint32_t max_connections = 64)
        : factory(std::move(wo))
        , group({1, max_connections})
        , server(group, *Endpoint::Tcp("127.0.0.1", 0), ServerOptions(handshake_timeout, std::move(layer)), rec, factory)
    {
        rec.on_message = Echo;
        CHECK(group.Start() == 0);
        CHECK(server.Start() == 0);
    }
    NetStats stats() const { return group.GetStats(); }
    std::uint64_t Failed(HandshakeFailure f) const { return stats().handshake_failed[static_cast<std::size_t>(f)]; }
    const Endpoint& ep() const { return server.endpoint(); }

    // 成员顺序：handler 与 factory 要比 group 活得久（group 析构时还会回调它们）。
    EventRecorder rec;
    WsServerCodecFactory factory;
    EventLoopGroup group;
    StreamServer server;
};

struct WsClientSide
{
    explicit WsClientSide(WsClientOptions o = {})
        : factory(std::move(o))
        , group({1, 64})
    {
        CHECK(group.Start() == 0);
    }
    EventRecorder rec;
    WsClientCodecFactory factory;
    EventLoopGroup group;
};

ReconnectPolicy FastPolicy() { return {20ms, 100ms, 2.0, 0.0}; }

WsOutFrame Bin(std::string payload) { return {2, true, std::move(payload)}; }

// 握手成功的对照客户端。
std::unique_ptr<WsRawClient> OpenRaw(const WsServer& s, bool fake_layer = false)
{
    auto c = std::make_unique<WsRawClient>(s.ep(), fake_layer);
    c->Send(WsHandshakeRequest());
    CHECK(c->ReadHead() == kWs101);
    return c;
}

struct TestContext final : ConnectionContext
{
    TestContext(std::string p, std::string t)
        : path(std::move(p))
        , token(std::move(t))
    {
    }
    std::string path;
    std::string token;
};

// ---- 服务端 ----

void TestUpgradeAndEcho()
{
    const LeakCheck leak;
    WsServer s;
    {
        WsRawClient c(s.ep());
        c.Send(WsHandshakeRequest());
        CHECK(c.ReadHead() == kWs101);
        s.rec.WaitConnected(0);
        CHECK(s.stats().handshaking_connections == 0);

        c.Send(WsFrame(2, "hello"));
        CHECK(c.ReadFrame() == Bin("hello"));
        c.Send(WsFrame(9, "pp")); // Ping 由 codec 回 Pong，业务看不见
        CHECK((c.ReadFrame() == WsOutFrame{10, true, "pp"}));
        c.Send(WsFrame(2, ""));
        CHECK(c.ReadFrame() == Bin(""));
        c.Send(WsFrame(2, std::string(300, 'x'))); // 4 字节头
        CHECK(c.ReadFrame() == Bin(std::string(300, 'x')));
        CHECK(s.rec.message_count() == 3);
    }
    const auto closed = s.rec.WaitClosed(0); // 对端不发 Close 帧直接断开
    CHECK(closed.reason == CloseReason::PeerClosed && closed.err == 0);
    s.rec.CheckInvariants();
}

// 握手完成的同时业务在 OnConnected 里就 Send：101 必须先到、连接状态必须已就绪（codec 在 HandshakeDone 之前进入 Open）。
void TestSendInsideOnConnected()
{
    const LeakCheck leak;
    WsServer s;
    s.rec.on_connected = [](IConnection& c) { Echo(c, "welcome"); };
    {
        WsRawClient c(s.ep());
        c.Send(WsHandshakeRequest());
        CHECK(c.ReadHead() == kWs101);
        CHECK(c.ReadFrame() == Bin("welcome"));
    }
    s.rec.WaitClosed(0);
    s.rec.CheckInvariants();
}

// 业务在 OnConnected 里就拒绝这条连接（例：账号被封、服务器满）：优雅关闭必须发出 WebSocket 的 Close(1001) 帧，
// 而不是只断 TCP。要求 codec 在 HandshakeDone（回调 OnConnected）之前就已进入 Open。服务端与库自己的客户端各测一遍。
void TestCloseGracefullyInsideOnConnected()
{
    const LeakCheck leak;
    {
        WsServer s;
        s.rec.on_connected = [](IConnection& c) { c.CloseGracefully(2s); };
        {
            WsRawClient c(s.ep());
            c.Send(WsHandshakeRequest());
            CHECK(c.ReadHead() == kWs101);
            CHECK(c.ReadFrame() == (WsOutFrame{8, true, WsCloseBody(1001)}));
            CHECK(c.ReadToEof().empty());
        }
        CHECK(s.rec.WaitClosed(0).reason == CloseReason::LocalClose);
        s.rec.CheckInvariants();
    }
    {
        WsServer s;
        WsClientSide cs;
        cs.rec.on_connected = [](IConnection& c) { c.CloseGracefully(2s); };
        {
            StreamClient c(cs.group, s.ep(), {}, FastPolicy(), cs.rec, cs.factory);
            CHECK(c.Start() == 0);
            // 客户端的 Close(1001) 到了服务端：服务端回显并带 1001 通知业务
            const auto sc = s.rec.WaitClosed(0);
            CHECK(sc.reason == CloseReason::PeerClosed && sc.err == 1001);
            c.Stop();
        }
    }
}

void TestPipelinedAndTrickled()
{
    const LeakCheck leak;
    WsServer s;
    {
        // 升级请求与两个帧在同一次 send 里到达
        WsRawClient c(s.ep());
        c.Send(WsHandshakeRequest() + WsFrame(2, "a") + WsFrame(2, "b"));
        CHECK(c.ReadHead() == kWs101);
        CHECK(c.ReadFrame() == Bin("a"));
        CHECK(c.ReadFrame() == Bin("b"));
    }
    for (const std::size_t chunk : {1, 3, 7})
    {
        // 一次几个字节：请求头、帧头、掩码键、payload 都被切碎
        WsRawClient c(s.ep());
        c.SendTrickled(WsHandshakeRequest() + WsFrame(2, "trickle-1", true, true, 0xDEADBEEF) + WsFrame(9, "pg") +
                           WsFrame(2, std::string(200, 'z'), true, true, 0x01020304),
                       chunk);
        CHECK(c.ReadHead() == kWs101);
        CHECK(c.ReadFrame() == Bin("trickle-1"));
        CHECK((c.ReadFrame() == WsOutFrame{10, true, "pg"}));
        CHECK(c.ReadFrame() == Bin(std::string(200, 'z')));
    }
    CHECK(WaitUntil([&] { return s.rec.closed_count() == 4; }));
    s.rec.CheckInvariants();
}

// 对端发起关闭：回显状态码，写完后服务端 shutdown(SHUT_WR)；OnClosed 带出对端的关闭码。
void TestPeerInitiatedClose()
{
    const LeakCheck leak;
    for (const int code : {1000, 4001})
    {
        WsServer s;
        {
            auto c = OpenRaw(s);
            s.rec.WaitConnected(0);
            c->Send(WsFrame(8, WsCloseBody(code, "bye")));
            CHECK(c->ReadFrame() == (WsOutFrame{8, true, WsCloseBody(code)}));
            CHECK(c->ReadToEof().empty()); // 服务端写完后关闭写方向
            c->Close();
        }
        const auto closed = s.rec.WaitClosed(0);
        CHECK(closed.reason == CloseReason::PeerClosed && closed.err == code);
        s.rec.CheckInvariants();
    }
}

void TestProtocolViolationClosesWithCode()
{
    const LeakCheck leak;
    struct Case
    {
        std::string bytes;
        int code;
        WsServerOptions opts;
    };
    WsServerOptions small;
    small.max_message_bytes = 100;
    WsServerOptions text;
    text.message_type = WsMessageType::Text;
    const std::vector<Case> cases = {
        {WsFrame(2, "x", true, /*mask=*/false), 1002, {}},   // 客户端没加掩码
        {WsFrame(1, "text"), 1003, {}},                      // Binary 端点收到 Text
        {WsHeaderOnly(2, 101), 1009, small},                 // 只发帧头：长度一到就拒绝，不等 payload
        {WsFrame(1, "\xC0\x80"), 1007, text},                // Text 里的非法 UTF-8
        {WsFrame(0, "orphan"), 1002, {}},                    // 没有在进行的分片
    };
    for (const Case& k : cases)
    {
        WsServer s(k.opts);
        {
            auto c = OpenRaw(s);
            s.rec.WaitConnected(0);
            c->Send(k.bytes);
            CHECK(c->ReadFrame() == (WsOutFrame{8, true, WsCloseBody(k.code)}));
            CHECK(c->ReadToEof().empty());
            c->Close();
        }
        const auto closed = s.rec.WaitClosed(0);
        CHECK(closed.reason == CloseReason::ProtocolError && closed.err == k.code);
        CHECK(s.rec.message_count() == 0);
        s.rec.CheckInvariants();
    }
}

// 被拒绝的升级：HTTP 错误应答，写完后关闭；没有 OnConnected / OnClosed，只记握手失败（Upgrade）。
void TestRejectedUpgrades()
{
    const LeakCheck leak;
    WsServerOptions o;
    o.path = "/game";
    o.health_path = "/healthz";
    WsServer s(o);
    const auto expect_reject = [&](const std::string& request, const std::string& status_prefix)
    {
        const auto before = s.Failed(HandshakeFailure::Upgrade);
        {
            WsRawClient c(s.ep());
            c.Send(request);
            CHECK(c.ReadHead().starts_with(status_prefix));
            CHECK(c.ReadToEof().empty());
            c.Close();
        }
        CHECK(WaitUntil([&] { return s.Failed(HandshakeFailure::Upgrade) == before + 1; }));
    };
    expect_reject(WsHandshakeRequest("/other"), "HTTP/1.1 404 ");
    expect_reject("GET /game HTTP/1.1\r\nHost: h\r\n\r\n", "HTTP/1.1 426 ");
    expect_reject("POST /game HTTP/1.1\r\nHost: h\r\n\r\n", "HTTP/1.1 405 ");
    expect_reject(WsHandshakeRequest("/game", "", "bad-key"), "HTTP/1.1 400 ");
    expect_reject("GET /game HTTP/1.1\r\nHost: h\r\nX:\r\n a\r\n\r\n", "HTTP/1.1 400 "); // obs-fold
    // 往明文端口发了 TLS ClientHello 之类的二进制（不含 NUL：字面量转 std::string 会在 NUL 处截断）
    expect_reject("\x16\x03\x01\x02\xFF binary-garbage-not-http\r\n\r\n", "HTTP/1.1 400 ");

    // 健康检查：200，写完后关闭，不算握手失败
    const auto failed_before = s.Failed(HandshakeFailure::Upgrade);
    {
        WsRawClient c(s.ep());
        c.Send("GET /healthz HTTP/1.1\r\nHost: lb\r\n\r\n");
        const std::string head = c.ReadHead();
        CHECK(head.starts_with("HTTP/1.1 200 OK\r\n"));
        CHECK(c.ReadToEof() == "ok");
        c.Close();
    }
    std::this_thread::sleep_for(100ms);
    CHECK(s.Failed(HandshakeFailure::Upgrade) == failed_before);
    CHECK(s.stats().handshaking_connections == 0);
    CHECK(s.rec.connected_count() == 0 && s.rec.closed_count() == 0);
}

// 慢速攻击：请求头只发了一半就不动了。按 handshake_timeout 关闭，记 Timeout；业务无感知。
void TestSlowlorisTimesOut()
{
    const LeakCheck leak;
    WsServer s({}, 200ms);
    {
        WsRawClient c(s.ep());
        c.Send("GET /ws HTTP/1.1\r\nHost: h");
        CHECK(WaitUntil([&] { return s.stats().handshaking_connections == 1; }));
        CHECK(c.raw().WaitClosed(5s));
        CHECK(WaitUntil([&] { return s.Failed(HandshakeFailure::Timeout) == 1; }));
        CHECK(s.rec.connected_count() == 0 && s.rec.closed_count() == 0);
        CHECK(s.stats().handshaking_connections == 0);
    }
}

// 升级之后握手阶段结束，不再受 handshake_timeout 约束：空闲的已建立连接不会被它关掉。
void TestEstablishedConnectionSurvivesHandshakeTimeout()
{
    const LeakCheck leak;
    WsServer s({}, 150ms);
    WsRawClient c(s.ep());
    c.Send(WsHandshakeRequest());
    CHECK(c.ReadHead() == kWs101);
    std::this_thread::sleep_for(400ms);
    c.Send(WsFrame(2, "still-here"));
    CHECK(c.ReadFrame() == Bin("still-here"));
    CHECK(s.Failed(HandshakeFailure::Timeout) == 0);
}

// 服务端优雅关闭：发 Close(1001)，写完后关闭写方向；对端应答并断开，OnClosed 是 LocalClose。
void TestServerGracefulClose()
{
    const LeakCheck leak;
    WsServer s;
    s.rec.on_message = [](IConnection& c, std::string_view m)
    {
        if (m == "close-me")
            c.CloseGracefully(2s);
        else
            Echo(c, m);
    };
    {
        auto c = OpenRaw(s);
        s.rec.WaitConnected(0);
        c->Send(WsFrame(2, "close-me"));
        CHECK(c->ReadFrame() == (WsOutFrame{8, true, WsCloseBody(1001)}));
        c->Send(WsFrame(8, WsCloseBody(1000))); // 应答
        CHECK(c->ReadToEof().empty());
        c->Close();
    }
    const auto closed = s.rec.WaitClosed(0);
    CHECK(closed.reason == CloseReason::LocalClose);
    s.rec.CheckInvariants();
}

// 同一块 SharedBuffer 发给多条连接：帧头只依赖长度，payload 共享，每条连接收到完全相同的帧。
void TestBroadcastSharedPayload()
{
    const LeakCheck leak;
    std::mutex mu;
    std::vector<IConnection*> conns;
    WsServer s;
    s.rec.on_connected = [&](IConnection& c)
    {
        const std::lock_guard lk(mu);
        conns.push_back(&c);
    };
    s.rec.on_closed = [&](IConnection& c, CloseReason)
    {
        const std::lock_guard lk(mu);
        std::erase(conns, &c);
    };
    s.rec.on_message = [&](IConnection& c, std::string_view m)
    {
        if (m != "bcast")
        {
            Echo(c, m);
            return;
        }
        const SharedBuffer shared = Buf("shared-payload");
        const std::lock_guard lk(mu);
        for (IConnection* x : conns)
            (void)x->Send(shared);
    };
    std::vector<std::unique_ptr<WsRawClient>> clients;
    for (std::size_t i = 0; i < 3; ++i)
    {
        clients.push_back(OpenRaw(s));
        s.rec.WaitConnected(i);
    }
    clients[0]->Send(WsFrame(2, "bcast"));
    for (auto& c : clients)
        CHECK(c->ReadFrame() == Bin("shared-payload"));
    clients.clear();
    CHECK(WaitUntil([&] { return s.rec.closed_count() == 3; }));
}

void TestLargeMessages()
{
    const LeakCheck leak;
    WsServerOptions o;
    o.max_message_bytes = 128 * 1024;
    WsServer s(o);
    {
        auto c = OpenRaw(s);
        for (const std::size_t n : {std::size_t{60000}, std::size_t{128 * 1024}}) // 后者恰好等于上限
        {
            std::string payload(n, '\0');
            for (std::size_t i = 0; i < n; ++i)
                payload[i] = static_cast<char>(i * 31 + 7);
            c->Send(WsFrame(2, payload, true, true, 0xCAFEBABE));
            CHECK(c->ReadFrame() == Bin(payload));
        }
        // 多一个字节：只发帧头就被拒绝
        c->Send(WsHeaderOnly(2, 128 * 1024 + 1));
        CHECK(c->ReadFrame() == (WsOutFrame{8, true, WsCloseBody(1009)}));
        CHECK(c->ReadToEof().empty());
    }
    const auto closed = s.rec.WaitClosed(0);
    CHECK(closed.reason == CloseReason::ProtocolError && closed.err == 1009);
}

// 升级回调把 path / token 解析成 context 装到连接上；OnConnected 里就能取到。
void TestUpgradeContextVisibleInOnConnected()
{
    const LeakCheck leak;
    WsServerOptions o;
    o.on_upgrade = [](const WsUpgradeRequest& r, WsUpgradeReply& reply) noexcept
    { reply.context = std::make_unique<TestContext>(std::string(r.path), std::string(r.Header("x-token"))); };
    WsServer s(o);
    std::mutex mu;
    std::string seen;
    s.rec.on_connected = [&](IConnection& c)
    {
        const auto* ctx = c.GetContext<TestContext>();
        const std::lock_guard lk(mu);
        seen = ctx != nullptr ? ctx->path + "|" + ctx->token : "(none)";
    };
    {
        WsRawClient c(s.ep());
        c.Send(WsHandshakeRequest("/lobby?x=1", "X-Token: secret\r\n"));
        CHECK(c.ReadHead() == kWs101);
        s.rec.WaitConnected(0);
    }
    const std::lock_guard lk(mu);
    CHECK(seen == "/lobby|secret");
}

// 很多连接同时握手、收发、断开：没有 fd / 线程泄漏，握手中的连接数回到 0。
void TestManyConnections()
{
    const LeakCheck leak;
    constexpr std::size_t kN = 100;
    WsServer s({}, 5s, nullptr, 256); // 默认的连接表只有 64 个槽
    {
        std::vector<std::unique_ptr<WsRawClient>> cs;
        for (std::size_t i = 0; i < kN; ++i)
        {
            cs.push_back(std::make_unique<WsRawClient>(s.ep()));
            cs.back()->Send(WsHandshakeRequest());
        }
        for (std::size_t i = 0; i < kN; ++i)
            CHECK(cs[i]->ReadHead() == kWs101);
        for (std::size_t i = 0; i < kN; ++i)
            cs[i]->Send(WsFrame(2, "n" + std::to_string(i), true, true, static_cast<std::uint32_t>(i * 2654435761u)));
        for (std::size_t i = 0; i < kN; ++i)
            CHECK(cs[i]->ReadFrame() == Bin("n" + std::to_string(i)));
        CHECK(s.stats().handshaking_connections == 0);
    }
    CHECK(WaitUntil([&] { return s.rec.closed_count() == kN; }));
    s.rec.CheckInvariants();
}

// ---- 库自己的客户端 ----

void TestLibraryClientEchoes()
{
    const LeakCheck leak;
    WsServerOptions so;
    so.max_message_bytes = 1 << 20;
    WsServer s(so);
    WsClientOptions co;
    co.host = "game.example.com";
    co.path = "/ws";
    co.max_message_bytes = 1 << 20;
    WsClientSide cs(co);
    {
        StreamClient c(cs.group, s.ep(), {}, FastPolicy(), cs.rec, cs.factory);
        CHECK(c.Start() == 0);
        cs.rec.WaitConnected(0);
        CHECK(c.handle().valid());
        std::size_t sent = 0;
        for (const std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{125}, std::size_t{126}, std::size_t{65535}, std::size_t{65536}, std::size_t{200000}}) // 200000 < 默认 high_water_mark(256KB)，仍是 10 字节帧头
        {
            std::string payload(n, '\0');
            for (std::size_t i = 0; i < n; ++i)
                payload[i] = static_cast<char>('a' + i % 23);
            CHECK(c.Send(Buf(payload)) == SendResult::Ok);
            ++sent;
            CHECK(cs.rec.WaitMessages(sent));
            CHECK(cs.rec.payloads().back() == payload);
        }
        CHECK(cs.group.GetStats().handshaking_connections == 0);
    }
    cs.rec.CheckInvariants();
    s.rec.CheckInvariants();
}

// 客户端发起优雅关闭：Close(1001) → 服务端回显并带 1001 通知业务 → 客户端 OnClosed 是 LocalClose。
void TestLibraryClientGracefulClose()
{
    const LeakCheck leak;
    WsServer s;
    WsClientSide cs;
    cs.rec.on_message = [](IConnection& c, std::string_view) { c.CloseGracefully(2s); };
    {
        StreamClient c(cs.group, s.ep(), {}, FastPolicy(), cs.rec, cs.factory);
        CHECK(c.Start() == 0);
        cs.rec.WaitConnected(0);
        CHECK(c.Send(Buf("trigger")) == SendResult::Ok); // 服务端回显 → 客户端 on_message → CloseGracefully
        const auto cc = cs.rec.WaitClosed(0);
        CHECK(cc.reason == CloseReason::LocalClose);
        const auto sc = s.rec.WaitClosed(0);
        CHECK(sc.reason == CloseReason::PeerClosed && sc.err == 1001);
        c.Stop();
    }
}

// 握手被服务端拒绝（升级回调回 403）：客户端按握手失败处理——退避重试、on_connect_failed 收到固定的 EPROTO，没有 OnConnected。
void TestLibraryClientRetriesAfterRejection()
{
    const LeakCheck leak;
    WsServerOptions so;
    so.on_upgrade = [](const WsUpgradeRequest&, WsUpgradeReply& reply) noexcept { reply.reject_status = 403; };
    WsServer s(so);
    WsClientSide cs;

    std::mutex mu;
    std::vector<std::pair<int, bool>> fails;
    StreamClientOptions co;
    co.on_connect_failed = [&](int e, bool retry) noexcept
    {
        const std::lock_guard lk(mu);
        fails.emplace_back(e, retry);
    };
    {
        StreamClient c(cs.group, s.ep(), std::move(co), FastPolicy(), cs.rec, cs.factory);
        CHECK(c.Start() == 0);
        CHECK(WaitUntil(
            [&]
            {
                const std::lock_guard lk(mu);
                return fails.size() >= 3;
            }));
    }
    {
        const std::lock_guard lk(mu);
        for (const auto& [e, retry] : fails)
            CHECK(e == EPROTO && retry);
    }
    CHECK(cs.rec.connected_count() == 0 && cs.rec.closed_count() == 0 && s.rec.connected_count() == 0);
    // 服务端的统计要等它读到客户端的 EOF 收尾之后才记，比客户端的失败回调晚一步：要等，不能直接比
    CHECK(WaitUntil([&] { return s.Failed(HandshakeFailure::Upgrade) >= 3; }));
    CHECK(cs.group.GetStats().handshake_failed[static_cast<std::size_t>(HandshakeFailure::Upgrade)] >= 3);
}

// 批内合并与变长帧头：一段 Send 里每条的帧头长度不同（2 / 4 / 10 字节，由 payload 长度决定），合成一次 sendmsg 时
// 每帧要带着自己的帧头；明文连接（Plain）与叠在层上的连接（Layered，wss 的形状）各走一遍。
void TestBatchedSendsKeepEveryFrameHeader()
{
    const LeakCheck leak;
    for (const bool layered : {false, true})
    {
        const auto layer = layered ? std::make_shared<FakeLayerFactory>() : nullptr;
        WsServerOptions so;
        so.max_message_bytes = 1 << 20;
        WsServer s(so, 5s, layer);
        s.rec.on_message = nullptr;
        auto c = OpenRaw(s, /*fake_layer=*/layered);
        const ConnectionHandle h = s.rec.WaitConnected(0);
        EventLoop& loop = GroupAccess::LoopAt(s.group, 0);

        std::vector<std::string> sent;
        {
            LoopGate gate(loop);
            // 0 / 1 / 125（2 字节头）、126 / 300 / 65535（4 字节头）、65536 / 70000（10 字节头）穿插
            for (const std::size_t n : {std::size_t{0}, std::size_t{5}, std::size_t{300}, std::size_t{1}, std::size_t{70000}, std::size_t{125},
                                        std::size_t{126}, std::size_t{65535}, std::size_t{65536}, std::size_t{2}, std::size_t{4000}})
            {
                sent.emplace_back(n, static_cast<char>('A' + n % 26));
                CHECK(h.Send(Buf(sent.back())) == SendResult::Ok);
            }
        }
        for (const std::string& p : sent)
            CHECK(c->ReadFrame() == Bin(p));
        CHECK(WaitUntil([&] { return loop.table().PendingBytes(h.slot()) == 0; }));
        CHECK(WaitUntil([&] { return s.stats().messages_out == sent.size(); }));
        c.reset();
        s.rec.WaitClosed(0);
    }
}

// 库自己的 ws 客户端（出站要掩码，走分层连接）成批发送：合并路径必须退回逐条，每一帧都要带掩码——
// 否则服务端按规范拒绝未掩码的客户端帧（1002）。回显回来的内容与顺序要对得上。
void TestBatchedSendsFromLibraryClientAreMasked()
{
    const LeakCheck leak;
    WsServerOptions so;
    so.max_message_bytes = 1 << 20;
    WsServer s(so);
    WsClientOptions co;
    co.max_message_bytes = 1 << 20;
    WsClientSide cs(co);
    {
        StreamClient c(cs.group, s.ep(), StreamClientOptions{}, FastPolicy(), cs.rec, cs.factory);
        CHECK(c.Start() == 0);
        cs.rec.WaitConnected(0);
        EventLoop& loop = GroupAccess::LoopAt(cs.group, 0);
        std::vector<std::string> sent;
        {
            LoopGate gate(loop);
            for (int i = 0; i < 12; ++i)
            {
                sent.emplace_back(static_cast<std::size_t>(i * 13 % 90) + 1, static_cast<char>('a' + i));
                CHECK(c.Send(Buf(sent.back())) == SendResult::Ok);
            }
        }
        CHECK(cs.rec.WaitMessages(sent.size(), 10s));
        CHECK(cs.rec.payloads() == sent);
        CHECK(s.rec.closed_count() == 0); // 没有因为未掩码的帧被服务端关掉
    }
    cs.rec.CheckInvariants();
    s.rec.CheckInvariants();
}

// ---- ws 叠在字节流层上（wss 的叠加方式）----

void TestWsOverStreamLayer()
{
    const LeakCheck leak;
    const auto layer = std::make_shared<FakeLayerFactory>();
    WsServerOptions so;
    so.max_message_bytes = 1 << 20;
    WsServer s(so, 5s, layer);
    {
        // 对照客户端穿过假层：线上每个字节都是异或过的，说明读写真的经过了层
        auto c = OpenRaw(s, /*fake_layer=*/true);
        s.rec.WaitConnected(0);
        c->Send(WsFrame(2, "through-the-layer"));
        CHECK(c->ReadFrame() == Bin("through-the-layer"));
        c->Send(WsFrame(9, "pg"));
        CHECK((c->ReadFrame() == WsOutFrame{10, true, "pg"}));
        c->Send(WsFrame(8, WsCloseBody(1000)));
        CHECK(c->ReadFrame() == (WsOutFrame{8, true, WsCloseBody(1000)}));
    }
    s.rec.WaitClosed(0);

    // 库自己的客户端：层的握手与 codec 的握手同时存在；客户端掩码走分层连接（encodes_payload + 层）
    WsClientOptions co;
    co.max_message_bytes = 1 << 20;
    WsClientSide cs(co);
    StreamClientOptions sco;
    sco.tls = layer;
    {
        StreamClient c(cs.group, s.ep(), std::move(sco), FastPolicy(), cs.rec, cs.factory);
        CHECK(c.Start() == 0);
        cs.rec.WaitConnected(0);
        std::size_t sent = 0;
        for (const std::size_t n : {std::size_t{0}, std::size_t{5}, std::size_t{126}, std::size_t{70000}})
        {
            const std::string payload(n, static_cast<char>('A' + n % 26));
            CHECK(c.Send(Buf(payload)) == SendResult::Ok);
            ++sent;
            CHECK(cs.rec.WaitMessages(sent));
            CHECK(cs.rec.payloads().back() == payload);
        }
    }
    cs.rec.CheckInvariants();
    s.rec.CheckInvariants();
    // 每个层都被销毁：服务端 2 条连接 + 客户端 1 条
    CHECK(WaitUntil([&] { return layer->counters.created.load() == layer->counters.destroyed.load(); }));
    CHECK(layer->counters.created.load() == 3);
}

} // namespace

int main()
{
    TestUpgradeAndEcho();
    TestSendInsideOnConnected();
    TestCloseGracefullyInsideOnConnected();
    TestPipelinedAndTrickled();
    TestPeerInitiatedClose();
    TestProtocolViolationClosesWithCode();
    TestRejectedUpgrades();
    TestSlowlorisTimesOut();
    TestEstablishedConnectionSurvivesHandshakeTimeout();
    TestServerGracefulClose();
    TestBroadcastSharedPayload();
    TestBatchedSendsKeepEveryFrameHeader();
    TestBatchedSendsFromLibraryClientAreMasked();
    TestLargeMessages();
    TestUpgradeContextVisibleInOnConnected();
    TestManyConnections();
    TestLibraryClientEchoes();
    TestLibraryClientGracefulClose();
    TestLibraryClientRetriesAfterRejection();
    TestWsOverStreamLayer();
    std::puts("ws_server_test: ok");
    return 0;
}
