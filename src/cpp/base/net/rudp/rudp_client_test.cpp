// RudpClient：用原始 UDP 对端冒充服务端，测客户端自己的行为：握手（Hello 内容与重传、Accept 校验、Reject 的错误码映射、
// 超时、服务端晚起）、心跳、Close 之后不重连、换票据重来、Stop / Start 的各种时序、空闲、析构与停机。
// 无测试框架依赖：失败即 abort；每个用例都带超时，卡死也会 abort。

#include "rudp_client.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <thread>

#include "event_loop_group.h"
#include "net_test_util.h"
#include "rudp_test_util.h"

using namespace breeze;
using namespace breeze::testing;

// 每个用例开始前打一行名字：卡住 / 失败时从 ctest 日志一眼看出是哪个。
#define RUN(test)                                                                                                      \
    do                                                                                                                 \
    {                                                                                                                  \
        std::fprintf(stderr, "[ RUN ] %s\n", #test);                                                                   \
        test();                                                                                                        \
    } while (0)

namespace
{

struct Hello
{
    std::uint32_t conv;
    rudp::Token token;
    std::uint8_t version;
    Endpoint from; // 客户端的源地址：往这里回包
};

// 假服务端收一个 Hello；超时返回空。别的类型的包（Ping 等）跳过。
std::optional<Hello> RecvHello(RawUdp& server, std::chrono::milliseconds timeout = 2s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        Endpoint from = Loopback();
        const auto dg = server.Recv(50ms, &from);
        if (!dg)
            continue;
        const auto p = rudp::Parse(*dg);
        if (p && p->type == rudp::PacketType::Hello)
            return Hello{p->conv, p->token, p->version, from};
    }
    return std::nullopt;
}

// 收一个指定类型的包；其它类型跳过。
std::optional<rudp::PacketType> RecvType(RawUdp& server,
                                         std::chrono::milliseconds timeout,
                                         std::uint32_t* conv = nullptr)
{
    const auto dg = server.Recv(timeout);
    if (!dg)
        return std::nullopt;
    const auto p = rudp::Parse(*dg);
    if (!p)
        return std::nullopt;
    if (conv != nullptr)
        *conv = p->conv;
    return p->type;
}

// 在 timeout 内收完所有包，返回其中某类型的个数。
std::size_t CountType(RawUdp& server, rudp::PacketType type, std::chrono::milliseconds timeout)
{
    std::size_t n = 0;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto dg = server.Recv(20ms);
        if (!dg)
            continue;
        const auto p = rudp::Parse(*dg);
        if (p && p->type == type)
            ++n;
    }
    return n;
}

struct Failures
{
    struct One
    {
        int err;
        bool will_retry;
    };
    std::mutex mu;
    std::vector<One> list;
    void Add(int e, bool r)
    {
        const std::lock_guard lk(mu);
        list.push_back({e, r});
    }
    std::size_t size()
    {
        const std::lock_guard lk(mu);
        return list.size();
    }
    One at(std::size_t i)
    {
        const std::lock_guard lk(mu);
        return list.at(i);
    }
};

struct Fixture
{
    EventLoopGroup group;
    EventRecorder rec;
    RawUdp server; // 冒充服务端
    Failures failures;
    std::uint32_t next_conv = 1000;

    explicit Fixture(EventLoopGroupOptions go = EventLoopGroupOptions{2, 256})
        : group(go)
    {
        CHECK(group.Start() == 0);
    }
    ~Fixture() { group.Shutdown(5s); }

    RudpTicket Ticket()
    {
        RudpTicket t;
        t.conv = ++next_conv;
        t.token.fill(static_cast<std::byte>(next_conv & 0xFF));
        return t;
    }

    RudpClientOptions Options(const RudpTicket& t)
    {
        RudpClientOptions o;
        o.ticket = t;
        o.ping_interval = 0ms; // 默认不发心跳：免得干扰对包的断言
        o.on_connect_failed = [this](int e, bool r) noexcept { failures.Add(e, r); };
        return o;
    }

    std::unique_ptr<RudpClient> MakeClient(RudpClientOptions o)
    {
        return std::make_unique<RudpClient>(group, *RudpEndpoint::From(server.local()), std::move(o), rec);
    }

    // 启动并完成握手：返回 Hello 里的客户端地址。
    Endpoint Connect(RudpClient& c, const RudpTicket& t, std::size_t connected_index = 0)
    {
        CHECK(c.Start() == 0);
        const auto h = RecvHello(server);
        CHECK(h.has_value() && h->conv == t.conv);
        server.SendTo(h->from, rudp::EncodeAccept(t.conv).view());
        rec.WaitConnected(connected_index);
        return h->from;
    }
};

// ---- 握手 ----

// Hello 的内容：conv / token 是票据的，版本是协议版本；Accept 之前每 hello_interval 重传一次；Accept 之后不再发。
void TestHelloContentAndRetransmit()
{
    Fixture f;
    const RudpTicket t = f.Ticket();
    auto o = f.Options(t);
    o.hello_interval = 100ms;
    auto c = f.MakeClient(std::move(o));
    CHECK(c->Start() == 0);
    const auto first = RecvHello(f.server);
    CHECK(first && first->conv == t.conv && first->token == t.token && first->version == rudp::kVersion);
    std::size_t more = 0;
    const auto until = std::chrono::steady_clock::now() + 650ms;
    while (std::chrono::steady_clock::now() < until)
    {
        if (const auto h = RecvHello(f.server, 200ms))
        {
            CHECK(h->conv == t.conv && h->token == t.token && h->from == first->from); // 同一个 socket 重传
            ++more;
        }
    }
    CHECK(more >= 4 && more <= 8); // 约每 100ms 一个
    f.server.SendTo(first->from, rudp::EncodeAccept(t.conv).view());
    f.rec.WaitConnected(0);
    f.server.Drain();
    CHECK(!RecvHello(f.server, 400ms).has_value()); // 连上之后不再发 Hello
    CHECK(f.failures.size() == 0);
}

// 握手成功：OnConnected、句柄有效、双向收发（假服务端用 KcpPeer 讲 KCP）。
void TestAcceptConnectsAndExchangesMessages()
{
    Fixture f;
    const RudpTicket t = f.Ticket();
    auto c = f.MakeClient(f.Options(t));
    CHECK(!c->handle().valid());
    CHECK(c->Send(Buf("x")) == SendResult::Closed); // 没连上
    const Endpoint client_ep = f.Connect(*c, t);
    CHECK(c->handle().valid() && c->handle() == f.rec.WaitConnected(0));
    CHECK(c->endpoint() == f.server.local() && c->server().endpoint() == f.server.local());

    KcpPeer srv(f.server, client_ep, t.conv);
    CHECK(c->Send(Buf("hello")) == SendResult::Ok);
    CHECK(srv.RunUntil([&] { return srv.messages().size() == 1; }));
    CHECK(srv.messages()[0] == "hello");
    srv.Send("world");
    CHECK(srv.RunUntil([&] { return f.rec.message_count() == 1; }));
    CHECK(f.rec.payloads()[0] == "world");
    CHECK(f.group.GetStats().connected == 1);
}

// Reject 的原因映射成 errno，will_retry 恒为 false；之后回到 Idle，可以再 Start。
void TestRejectMapping()
{
    struct Case
    {
        std::uint8_t reason;
        int err;
    };
    const Case cases[] = {{static_cast<std::uint8_t>(rudp::RejectReason::Ticket), EACCES},
                          {static_cast<std::uint8_t>(rudp::RejectReason::Busy), EBUSY},
                          {static_cast<std::uint8_t>(rudp::RejectReason::Filtered), EPERM},
                          {static_cast<std::uint8_t>(rudp::RejectReason::Version), EPROTO},
                          {static_cast<std::uint8_t>(rudp::RejectReason::Stopping), ESHUTDOWN},
                          {99, ECONNREFUSED}};
    Fixture f;
    auto c = f.MakeClient(f.Options(f.Ticket()));
    std::size_t n = 0;
    for (const Case& k : cases)
    {
        CHECK(c->Start() == 0);
        const auto h = RecvHello(f.server);
        CHECK(h.has_value());
        auto reject = rudp::EncodeReject(rudp::RejectReason::Ticket);
        reject.bytes[1] = static_cast<std::byte>(k.reason);
        f.server.SendTo(h->from, reject.view());
        CHECK(WaitUntil([&] { return f.failures.size() == n + 1; }));
        CHECK(f.failures.at(n).err == k.err && !f.failures.at(n).will_retry);
        ++n;
    }
    CHECK(f.rec.connected_count() == 0 && !c->handle().valid());
}

// 超时：服务端一直不理，connect_timeout 到了报 ETIMEDOUT；之后不再发 Hello。
void TestHandshakeTimeout()
{
    Fixture f;
    const RudpTicket t = f.Ticket();
    auto o = f.Options(t);
    o.connect_timeout = 500ms;
    o.hello_interval = 100ms;
    auto c = f.MakeClient(std::move(o));
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(c->Start() == 0);
    CHECK(WaitUntil([&] { return f.failures.size() == 1; }, 5s));
    const auto took = std::chrono::steady_clock::now() - t0;
    CHECK(took >= 450ms && took < 2s);
    CHECK(f.failures.at(0).err == ETIMEDOUT && !f.failures.at(0).will_retry);
    // 放弃握手时告诉服务端（它那边可能已经建好会话、只是 Accept 丢了）：最后一个包是 Close(conv)
    bool got_close = false;
    while (const auto dg = f.server.Recv(100ms))
    {
        const auto p = rudp::Parse(*dg);
        got_close = got_close || (p && p->type == rudp::PacketType::Close && p->conv == t.conv);
    }
    CHECK(got_close);
    CHECK(!RecvHello(f.server, 400ms).has_value());
    CHECK(f.rec.connected_count() == 0);
}

// 端口没人监听：握手期间的 ECONNREFUSED 不算失败，一直重试到超时，超时时报最后的 socket 错误。
void TestConnRefusedReportedAtTimeout()
{
    Fixture f;
    std::uint16_t dead_port;
    {
        RawUdp temp;
        dead_port = temp.local().port();
    }
    auto o = f.Options(f.Ticket());
    o.connect_timeout = 600ms;
    o.hello_interval = 100ms;
    RudpClient c(f.group, RudpLoopback(dead_port), std::move(o), f.rec);
    CHECK(c.Start() == 0);
    CHECK(WaitUntil([&] { return f.failures.size() == 1; }, 5s));
    CHECK(f.failures.at(0).err == ECONNREFUSED);
}

// 服务端晚起：先被 ECONNREFUSED，窗口内服务端起来后重传的 Hello 就能握上。
void TestServerStartsLate()
{
    Fixture f;
    std::uint16_t port;
    {
        RawUdp temp;
        port = temp.local().port();
    }
    const RudpTicket t = f.Ticket();
    auto o = f.Options(t);
    o.connect_timeout = 5s;
    o.hello_interval = 100ms;
    RudpClient c(f.group, RudpLoopback(port), std::move(o), f.rec);
    CHECK(c.Start() == 0);
    std::this_thread::sleep_for(400ms); // 这期间 Hello 都撞上 ICMP 不可达
    CHECK(f.failures.size() == 0);
    RawUdp late(port);
    const auto h = RecvHello(late, 3s);
    CHECK(h.has_value() && h->conv == t.conv);
    late.SendTo(h->from, rudp::EncodeAccept(t.conv).view());
    CHECK(f.rec.WaitConnected(0).valid());
    CHECK(f.failures.size() == 0);
}

// 握手期间收到的不合格的包都不理：conv / 版本不对的 Accept、垃圾、比 Accept 先到的数据；之后真的 Accept 照样连上。
void TestBadRepliesIgnoredWhileHandshaking()
{
    Fixture f;
    const RudpTicket t = f.Ticket();
    auto c = f.MakeClient(f.Options(t));
    CHECK(c->Start() == 0);
    const auto h = RecvHello(f.server);
    CHECK(h.has_value());

    f.server.SendTo(h->from, rudp::EncodeAccept(t.conv + 1).view()); // conv 不对
    auto wrong_version = rudp::EncodeAccept(t.conv);
    wrong_version.bytes[1] = std::byte{rudp::kVersion + 1};
    f.server.SendTo(h->from, wrong_version.view());
    f.server.SendTo(h->from, "garbage that is not a packet");
    f.server.SendTo(h->from, rudp::EncodePong(t.conv).view());
    Bytes data(rudp::kDataPrefix + rudp::kKcpHeader, std::byte{0});
    data[0] = rudp::DataHeader();
    f.server.SendTo(h->from, data); // 数据比 Accept 先到
    std::this_thread::sleep_for(300ms);
    CHECK(f.rec.connected_count() == 0 && f.failures.size() == 0);
    CHECK(f.group.GetStats().datagrams_dropped >= 5);

    f.server.SendTo(h->from, rudp::EncodeAccept(t.conv).view());
    CHECK(f.rec.WaitConnected(0).valid());
}

// 握手用的定时器在连通时都要撤掉：连接活得比 connect_timeout 久，不会被迟来的"握手超时"拆掉。
void TestConnectionOutlivesConnectTimeout()
{
    Fixture f;
    const RudpTicket t = f.Ticket();
    auto o = f.Options(t);
    o.connect_timeout = 300ms;
    o.hello_interval = 50ms;
    auto c = f.MakeClient(std::move(o));
    f.Connect(*c, t);
    std::this_thread::sleep_for(900ms); // 远超 connect_timeout
    CHECK(f.rec.closed_count() == 0 && f.failures.size() == 0 && c->handle().valid());
    f.server.Drain();
    CHECK(!RecvHello(f.server, 200ms).has_value()); // Hello 重传也停了
    CHECK(c->Send(Buf("still here")) == SendResult::Ok);
}

// 连通之后，多出来的 Accept（Hello 重传与 Accept 交叉）、Reject 都不影响连接。
void TestExtraAcceptAndRejectIgnoredWhenConnected()
{
    Fixture f;
    const RudpTicket t = f.Ticket();
    auto c = f.MakeClient(f.Options(t));
    const Endpoint client_ep = f.Connect(*c, t);
    const auto dropped = f.group.GetStats().datagrams_dropped;
    f.server.SendTo(client_ep, rudp::EncodeAccept(t.conv).view());
    f.server.SendTo(client_ep, rudp::EncodeReject(rudp::RejectReason::Ticket).view());
    std::this_thread::sleep_for(300ms);
    CHECK(f.rec.closed_count() == 0 && c->handle().valid());
    CHECK(f.failures.size() == 0);
    // 多余的 Accept 是正常现象、不计丢弃；Reject 在连通后没有意义，计入丢弃
    CHECK(f.group.GetStats().datagrams_dropped == dropped + 1);
}

// ---- 心跳 ----

void TestPing()
{
    Fixture f;
    const RudpTicket t = f.Ticket();
    auto o = f.Options(t);
    o.ping_interval = 100ms;
    auto c = f.MakeClient(std::move(o));
    const Endpoint client_ep = f.Connect(*c, t);
    f.server.Drain();
    std::size_t pings = 0;
    const auto until = std::chrono::steady_clock::now() + 650ms;
    while (std::chrono::steady_clock::now() < until)
    {
        std::uint32_t conv = 0;
        const auto type = RecvType(f.server, 100ms, &conv);
        if (type == rudp::PacketType::Ping)
        {
            CHECK(conv == t.conv);
            ++pings;
        }
    }
    CHECK(pings >= 4 && pings <= 8);
    (void)client_ep;
}

void TestNoPingWhenDisabled()
{
    Fixture f;
    const RudpTicket t = f.Ticket();
    auto c = f.MakeClient(f.Options(t)); // ping_interval = 0
    f.Connect(*c, t);
    f.server.Drain();
    CHECK(CountType(f.server, rudp::PacketType::Ping, 500ms) == 0);
}

// ---- 断开与重来 ----

// 服务端发 Close：以 PeerClosed 结束，句柄失效；不自动重连（票据只能用一次）。
void TestServerCloseDoesNotReconnect()
{
    Fixture f;
    const RudpTicket t = f.Ticket();
    auto c = f.MakeClient(f.Options(t));
    const Endpoint client_ep = f.Connect(*c, t);
    f.server.SendTo(client_ep, rudp::EncodeClose(t.conv, 0).view());
    const auto closed = f.rec.WaitClosed(0);
    CHECK(closed.reason == CloseReason::PeerClosed);
    CHECK(!c->handle().valid() && c->Send(Buf("x")) == SendResult::Closed);
    f.server.Drain();
    CHECK(!RecvHello(f.server, 700ms).has_value()); // 没有重连
    CHECK(f.rec.connected_count() == 1);
    f.rec.CheckInvariants();
}

// 换票据重来：SetTicket 之后 Start，Hello 带新 conv，连得上；新连接是新的句柄。
void TestNewTicketAfterClose()
{
    Fixture f;
    const RudpTicket t1 = f.Ticket(), t2 = f.Ticket();
    auto c = f.MakeClient(f.Options(t1));
    const Endpoint ep1 = f.Connect(*c, t1);
    const ConnectionHandle h1 = f.rec.WaitConnected(0);
    f.server.SendTo(ep1, rudp::EncodeClose(t1.conv, 0).view());
    f.rec.WaitClosed(0);

    c->SetTicket(t2);
    f.Connect(*c, t2, 1);
    const ConnectionHandle h2 = f.rec.WaitConnected(1);
    CHECK(h1 != h2 && c->handle() == h2);
    CHECK(f.rec.connected_count() == 2);
    f.rec.CheckInvariants();
}

void TestStartValidation()
{
    Fixture f;
    {
        RudpClientOptions o = f.Options(RudpTicket{}); // 票据无效（conv 为 0）
        auto c = f.MakeClient(std::move(o));
        CHECK(c->Start() == EINVAL);
        c->SetTicket(f.Ticket()); // 补上就可以
        CHECK(c->Start() == 0);
        CHECK(RecvHello(f.server).has_value());
    }
    const auto bad = [&](auto mutate)
    {
        RudpClientOptions o = f.Options(f.Ticket());
        mutate(o);
        auto c = f.MakeClient(std::move(o));
        CHECK(c->Start() == EINVAL);
    };
    bad([](RudpClientOptions& o) { o.rudp.mtu = 100; });
    bad([](RudpClientOptions& o) { o.connect_timeout = 0ms; });
    bad([](RudpClientOptions& o) { o.hello_interval = 0ms; });
    bad([](RudpClientOptions& o) { o.ping_interval = -1ms; });
}

// group 已停机：Start 返回 ESHUTDOWN，不是编程错误。
void TestStartAfterShutdown()
{
    EventLoopGroup group(EventLoopGroupOptions{1, 64});
    CHECK(group.Start() == 0);
    EventRecorder rec;
    RawUdp server;
    RudpTicket t;
    t.conv = 5;
    RudpClientOptions o;
    o.ticket = t;
    RudpClient c(group, *RudpEndpoint::From(server.local()), std::move(o), rec);
    group.Shutdown(1s);
    CHECK(c.Start() == ESHUTDOWN);
}

// 握手中 Stop：不回调 on_connect_failed，不再发 Hello；可以再 Start。
void TestStopDuringHandshake()
{
    Fixture f;
    const RudpTicket t = f.Ticket(); // 下面 got_close 要对上它
    auto o = f.Options(t);
    o.hello_interval = 100ms;
    o.connect_timeout = 400ms; // 下面等得比它久：Stop 之后超时定时器不能再触发 on_connect_failed
    auto c = f.MakeClient(std::move(o));
    CHECK(c->Start() == 0);
    CHECK(RecvHello(f.server).has_value());
    c->Stop();
    std::this_thread::sleep_for(100ms);
    bool got_close = false; // 放弃握手：告诉服务端
    while (const auto dg = f.server.Recv(100ms))
    {
        const auto p = rudp::Parse(*dg);
        got_close = got_close || (p && p->type == rudp::PacketType::Close && p->conv == t.conv);
    }
    CHECK(got_close);
    CHECK(!RecvHello(f.server, 500ms).has_value());
    CHECK(f.failures.size() == 0 && f.rec.connected_count() == 0);

    CHECK(c->Start() == 0);
    CHECK(RecvHello(f.server).has_value());
}

// 连通后 Stop：OnClosed(LocalClose)，向服务端发 3 个 Close，句柄失效。
void TestStopWhenConnected()
{
    Fixture f;
    const RudpTicket t = f.Ticket();
    auto c = f.MakeClient(f.Options(t));
    f.Connect(*c, t);
    f.server.Drain();
    c->Stop();
    CHECK(f.rec.WaitClosed(0).reason == CloseReason::LocalClose);
    CHECK(!c->handle().valid() && c->Send(Buf("x")) == SendResult::Closed);
    std::size_t closes = 0;
    const auto until = std::chrono::steady_clock::now() + 500ms;
    while (std::chrono::steady_clock::now() < until)
    {
        std::uint32_t conv = 0;
        if (RecvType(f.server, 50ms, &conv) == rudp::PacketType::Close)
        {
            CHECK(conv == t.conv);
            ++closes;
        }
    }
    CHECK(closes == 3);
    f.rec.CheckInvariants();
}

// 先 Stop 再立刻 Start：新的握手要等旧连接的 OnClosed 之后才开始，两条连接不并存。
void TestStopThenStartDoesNotOverlap()
{
    Fixture f;
    const RudpTicket t1 = f.Ticket(), t2 = f.Ticket();
    auto c = f.MakeClient(f.Options(t1));
    f.Connect(*c, t1);
    f.server.Drain();
    c->Stop();
    c->SetTicket(t2);
    CHECK(c->Start() == 0);
    const auto h = RecvHello(f.server);
    CHECK(h.has_value() && h->conv == t2.conv);
    CHECK(f.rec.closed_count() == 1); // 收到新 Hello 时旧连接已经结束
    f.server.SendTo(h->from, rudp::EncodeAccept(t2.conv).view());
    CHECK(f.rec.WaitConnected(1).valid());
    CHECK(f.rec.connected_count() == 2);
    f.rec.CheckInvariants();
}

// 回调里直接 Start()（文档允许）：on_connect_failed 里重来。
void TestRestartFromConnectFailedCallback()
{
    Fixture f;
    std::atomic<int> failed{0};
    RudpClient* self = nullptr;
    auto o = f.Options(f.Ticket());
    o.on_connect_failed = [&](int, bool) noexcept
    {
        if (failed.fetch_add(1) == 0)
            CHECK(self->Start() == 0);
    };
    auto c = f.MakeClient(std::move(o));
    self = c.get();
    CHECK(c->Start() == 0);
    auto h = RecvHello(f.server);
    CHECK(h.has_value());
    f.server.SendTo(h->from, rudp::EncodeReject(rudp::RejectReason::Busy).view());
    h = RecvHello(f.server); // 回调里重启，又来一个 Hello
    CHECK(h.has_value());
    CHECK(failed.load() == 1);
}

// ---- 空闲 ----

// 开了 idle_timeout、不发心跳、服务端又不出声：以 IdleTimeout 关闭，并发 Close。
void TestIdleTimeoutWhenSilent()
{
    Fixture f;
    const RudpTicket t = f.Ticket();
    auto o = f.Options(t);
    o.connection.idle_timeout = 500ms;
    auto c = f.MakeClient(std::move(o));
    f.Connect(*c, t);
    f.server.Drain();
    const auto closed = f.rec.WaitClosed(0, 5s);
    CHECK(closed.reason == CloseReason::IdleTimeout);
    CHECK(CountType(f.server, rudp::PacketType::Close, 300ms) >= 1);
}

// 服务端对心跳回 Pong：持续收到包，不会空闲超时。
void TestPongKeepsAlive()
{
    Fixture f;
    const RudpTicket t = f.Ticket();
    auto o = f.Options(t);
    o.connection.idle_timeout = 500ms;
    o.ping_interval = 100ms;
    auto c = f.MakeClient(std::move(o));
    const Endpoint client_ep = f.Connect(*c, t);
    const auto until = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < until)
    {
        std::uint32_t conv = 0;
        if (RecvType(f.server, 50ms, &conv) == rudp::PacketType::Ping)
            f.server.SendTo(client_ep, rudp::EncodePong(conv).view());
    }
    CHECK(f.rec.closed_count() == 0 && c->handle().valid());
}

// 连通之后的 ICMP 不可达（服务端进程没了）不关会话：偶发的不可达不该断线，断线靠 Close / 空闲 / 链路判死。
void TestIcmpWhenConnectedDoesNotClose()
{
    EventLoopGroup group(EventLoopGroupOptions{1, 64});
    CHECK(group.Start() == 0);
    EventRecorder rec;
    auto server = std::make_unique<RawUdp>();
    RudpTicket t;
    t.conv = 77;
    t.token.fill(std::byte{1});
    RudpClientOptions o;
    o.ticket = t;
    o.ping_interval = 50ms;
    RudpClient c(group, *RudpEndpoint::From(server->local()), std::move(o), rec);
    CHECK(c.Start() == 0);
    const auto h = RecvHello(*server);
    CHECK(h.has_value());
    server->SendTo(h->from, rudp::EncodeAccept(t.conv).view());
    rec.WaitConnected(0);
    server.reset(); // 之后每个 Ping 都换来一个 ICMP 不可达
    std::this_thread::sleep_for(600ms);
    CHECK(rec.closed_count() == 0 && c.handle().valid());
    group.Shutdown(2s);
}

// ---- 析构与停机 ----

// 析构时 group 还在跑、连接还活着：同步关掉并等 OnClosed 回调完，不崩、不泄漏、服务端收到 Close。
void TestDestroyWhileConnected()
{
    LeakCheck leak;
    EventLoopGroup group(EventLoopGroupOptions{2, 64});
    CHECK(group.Start() == 0);
    EventRecorder rec;
    RawUdp server;
    RudpTicket t;
    t.conv = 9;
    t.token.fill(std::byte{2});
    auto dtor_start = std::chrono::steady_clock::now();
    {
        RudpClientOptions o;
        o.ticket = t;
        RudpClient c(group, *RudpEndpoint::From(server.local()), std::move(o), rec);
        CHECK(c.Start() == 0);
        const auto h = RecvHello(server);
        CHECK(h.has_value());
        server.SendTo(h->from, rudp::EncodeAccept(t.conv).view());
        rec.WaitConnected(0);
        dtor_start = std::chrono::steady_clock::now();
    } // 析构
    CHECK(std::chrono::steady_clock::now() - dtor_start < 2s); // 不靠"等 5 秒超时"才返回
    CHECK(rec.closed_count() == 1 && rec.WaitClosed(0).reason == CloseReason::LocalClose);
    rec.CheckInvariants();
    CHECK(CountType(server, rudp::PacketType::Close, 400ms) >= 1);
    group.Shutdown(1s);
}

// 握手中析构：不崩、不泄漏，之后不再有 Hello，也没有任何回调。
void TestDestroyDuringHandshake()
{
    LeakCheck leak;
    EventLoopGroup group(EventLoopGroupOptions{1, 64});
    CHECK(group.Start() == 0);
    EventRecorder rec;
    RawUdp server;
    {
        RudpTicket t;
        t.conv = 11;
        t.token.fill(std::byte{3});
        RudpClientOptions o;
        o.ticket = t;
        o.hello_interval = 50ms;
        RudpClient c(group, *RudpEndpoint::From(server.local()), std::move(o), rec);
        CHECK(c.Start() == 0);
        CHECK(RecvHello(server).has_value());
    }
    server.Drain();
    CHECK(!RecvHello(server, 400ms).has_value());
    CHECK(rec.connected_count() == 0 && rec.closed_count() == 0);
    group.Shutdown(1s);
}

// group 停机：握手中的放弃（无回调），连通的优雅关闭（OnClosed）；停机之后 Start 得到 ESHUTDOWN。
void TestGroupShutdown()
{
    LeakCheck leak;
    EventLoopGroup group(EventLoopGroupOptions{2, 64});
    CHECK(group.Start() == 0);
    EventRecorder rec;
    RawUdp server;
    RudpTicket t1, t2;
    t1.conv = 21;
    t1.token.fill(std::byte{4});
    t2.conv = 22;
    t2.token.fill(std::byte{5});
    RudpClientOptions o1, o2;
    o1.ticket = t1;
    o2.ticket = t2;
    RudpClient connected(group, *RudpEndpoint::From(server.local()), std::move(o1), rec);
    RudpClient pending(group, *RudpEndpoint::From(server.local()), std::move(o2), rec);
    CHECK(connected.Start() == 0);
    const auto h = RecvHello(server);
    CHECK(h.has_value() && h->conv == t1.conv);
    server.SendTo(h->from, rudp::EncodeAccept(t1.conv).view());
    rec.WaitConnected(0);
    CHECK(pending.Start() == 0);
    CHECK(RecvHello(server).has_value()); // pending 在握手中

    group.Shutdown(2s);
    CHECK(rec.closed_count() == 1 && rec.WaitClosed(0).reason == CloseReason::LocalClose);
    CHECK(connected.Start() == ESHUTDOWN && pending.Start() == ESHUTDOWN);
    rec.CheckInvariants();
}

// 握手失败 / 重来反复多次：fd 与线程都不泄漏（每次失败都关了 socket）。
void TestNoLeakAcrossRepeatedHandshakes()
{
    LeakCheck leak;
    {
        Fixture f;
        auto o = f.Options(f.Ticket());
        o.connect_timeout = 100ms;
        o.hello_interval = 30ms;
        auto c = f.MakeClient(std::move(o));
        for (int i = 0; i < 20; ++i)
        {
            CHECK(c->Start() == 0);
            CHECK(WaitUntil([&] { return f.failures.size() == static_cast<std::size_t>(i) + 1; }));
        }
        const int fds = CountEntries("/proc/self/fd");
        for (int i = 0; i < 5; ++i)
        {
            CHECK(c->Start() == 0);
            CHECK(WaitUntil([&] { return f.failures.size() == static_cast<std::size_t>(i) + 21; }));
        }
        CHECK(CountEntries("/proc/self/fd") <= fds); // 失败之后 socket 都关了，没有随次数增长
    }
}

} // namespace

int main()
{
    RUN(TestHelloContentAndRetransmit);
    RUN(TestAcceptConnectsAndExchangesMessages);
    RUN(TestRejectMapping);
    RUN(TestHandshakeTimeout);
    RUN(TestConnRefusedReportedAtTimeout);
    RUN(TestServerStartsLate);
    RUN(TestBadRepliesIgnoredWhileHandshaking);
    RUN(TestConnectionOutlivesConnectTimeout);
    RUN(TestExtraAcceptAndRejectIgnoredWhenConnected);
    RUN(TestPing);
    RUN(TestNoPingWhenDisabled);
    RUN(TestServerCloseDoesNotReconnect);
    RUN(TestNewTicketAfterClose);
    RUN(TestStartValidation);
    RUN(TestStartAfterShutdown);
    RUN(TestStopDuringHandshake);
    RUN(TestStopWhenConnected);
    RUN(TestStopThenStartDoesNotOverlap);
    RUN(TestRestartFromConnectFailedCallback);
    RUN(TestIdleTimeoutWhenSilent);
    RUN(TestPongKeepsAlive);
    RUN(TestIcmpWhenConnectedDoesNotClose);
    RUN(TestDestroyWhileConnected);
    RUN(TestDestroyDuringHandshake);
    RUN(TestGroupShutdown);
    RUN(TestNoLeakAcrossRepeatedHandshakes);
    std::puts("rudp_client_test: ok");
    return 0;
}
