// RUdpServer：用原始 UDP 对端（RawUdp / KcpPeer）直接和服务端讲协议，不经 RUdpClient，测服务端自己的行为：
// 握手与票据（一次性、过期、token 错、版本、过滤、表满、重传的 Hello）、按来源固定会话（陌生来源操纵不了会话）、
// 畸形包、心跳、Close、空闲、Stop、析构、统计。
// 无测试框架依赖：失败即 abort；每个用例都带超时，卡死也会 abort。

#include "base/net/rudp/rudp_server.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

#include "base/net/loop/event_loop_group.h"
#include "base/net/loop/group_access.h"
#include "base/net/testing/net_test_util.h"
#include "base/net/testing/rudp_test_util.h"

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

struct Reply
{
    rudp::PacketType type;
    std::uint8_t version;
    std::uint32_t conv;
    std::uint8_t reason;
};

std::optional<Reply> RecvReply(RawUdp& raw, std::chrono::milliseconds timeout = 2s)
{
    const auto dg = raw.Recv(timeout);
    if (!dg)
        return std::nullopt;
    const auto p = rudp::Parse(*dg);
    CHECK(p.has_value());
    return Reply{p->type, p->version, p->conv, p->reason};
}

void SendHello(RawUdp& raw, const Endpoint& to, const RUdpTicket& t)
{
    raw.SendTo(to, rudp::EncodeHello(t.conv, t.token).view());
}

bool IsReject(const std::optional<Reply>& r, rudp::RejectReason why)
{
    return r && r->type == rudp::PacketType::Reject && r->reason == static_cast<std::uint8_t>(why);
}

struct Fixture
{
    EventLoopGroup group;
    EventRecorder rec;
    std::unique_ptr<RUdpServer> server;

    explicit Fixture(RUdpServerOptions o = {}, EventLoopGroupOptions go = EventLoopGroupOptions{2, 256})
        : group(go)
    {
        CHECK(group.Start() == 0);
        server = std::make_unique<RUdpServer>(group, RUdpLoopback(), std::move(o), rec);
        CHECK(server->Start() == 0);
    }
    ~Fixture()
    {
        server.reset();
        group.Shutdown(5s);
    }

    const Endpoint& ep() const { return server->endpoint(); }
    RUdpTicket Ticket(std::chrono::milliseconds ttl = 30s)
    {
        const auto t = server->IssueTicket(ttl);
        CHECK(t.has_value());
        return *t;
    }
    NetStats stats() const { return group.GetStats(); }

    // 所有 loop 都跑过一遍任务：此前 Post 的任务（比如 Stop）已经执行。
    void Barrier()
    {
        for (std::size_t i = 0; i < GroupAccess::LoopCount(group); ++i)
            RunOnLoop(GroupAccess::LoopAt(group, i), [] {});
    }
};

// ---- 握手 ----

void TestHandshakeAccept()
{
    Fixture f;
    RawUdp raw;
    const RUdpTicket t = f.Ticket();
    SendHello(raw, f.ep(), t);
    const auto r = RecvReply(raw);
    CHECK(r && r->type == rudp::PacketType::Accept && r->conv == t.conv && r->version == rudp::kVersion);
    f.rec.WaitConnected(0);
    CHECK(f.stats().accepted == 1 && f.stats().connections == 1);
    CHECK(f.server->pending_tickets() == 0); // 票据用掉了
}

// 没登记的票据：Reject(Ticket)，不建会话，计数。
void TestUnknownTicketRejected()
{
    Fixture f;
    RawUdp raw;
    RUdpTicket fake;
    fake.conv = 12345;
    fake.token.fill(std::byte{7});
    SendHello(raw, f.ep(), fake);
    CHECK(IsReject(RecvReply(raw), rudp::RejectReason::Ticket));
    CHECK(f.rec.connected_count() == 0);
    CHECK(f.stats().rejected_ticket == 1 && f.stats().accepted == 0 && f.stats().connections == 0);
}

// token 错：拒绝，且不烧掉合法票据；随后用对的 token 仍能握手。
void TestWrongTokenRejectedThenRightOne()
{
    Fixture f;
    RawUdp raw;
    RUdpTicket t = f.Ticket();
    RUdpTicket forged = t;
    forged.token[3] ^= std::byte{0x40};
    SendHello(raw, f.ep(), forged);
    CHECK(IsReject(RecvReply(raw), rudp::RejectReason::Ticket));
    CHECK(f.server->pending_tickets() == 1);
    SendHello(raw, f.ep(), t);
    const auto r = RecvReply(raw);
    CHECK(r && r->type == rudp::PacketType::Accept && r->conv == t.conv);
}

// 一次性：第二个来源出示同一张票据被拒。
void TestTicketSingleUse()
{
    Fixture f;
    RawUdp a, b;
    const RUdpTicket t = f.Ticket();
    SendHello(a, f.ep(), t);
    CHECK(RecvReply(a)->type == rudp::PacketType::Accept);
    SendHello(b, f.ep(), t);
    CHECK(IsReject(RecvReply(b), rudp::RejectReason::Ticket));
    CHECK(f.rec.connected_count() == 1);
}

void TestTicketExpired()
{
    Fixture f;
    RawUdp raw;
    const RUdpTicket t = f.Ticket(50ms);
    std::this_thread::sleep_for(120ms);
    SendHello(raw, f.ep(), t);
    CHECK(IsReject(RecvReply(raw), rudp::RejectReason::Ticket));
    CHECK(f.rec.connected_count() == 0);
}

// 别处签发的票据经 AddTicket 登记也能用。
void TestAddTicket()
{
    Fixture f;
    RawUdp raw;
    RUdpTicket t;
    t.conv = 0xABCD1234u;
    t.token.fill(std::byte{0x5C});
    CHECK(f.server->AddTicket(t, 10s));
    CHECK(!f.server->AddTicket(t, 10s)); // conv 已被占用
    CHECK(f.server->pending_tickets() == 1);
    SendHello(raw, f.ep(), t);
    const auto r = RecvReply(raw);
    CHECK(r && r->type == rudp::PacketType::Accept && r->conv == t.conv);
}

void TestTicketLimits()
{
    RUdpServerOptions o;
    o.max_pending_tickets = 2;
    Fixture f(std::move(o));
    CHECK(f.server->IssueTicket(10s).has_value());
    CHECK(f.server->IssueTicket(10s).has_value());
    CHECK(!f.server->IssueTicket(10s).has_value()); // 满了
    CHECK(!f.server->IssueTicket(0ms).has_value()); // ttl 无效
    CHECK(f.server->pending_tickets() == 2);
}

// Hello 重传（客户端没收到 Accept）：重发 Accept，不建第二个会话。
void TestDuplicateHelloResendsAccept()
{
    Fixture f;
    RawUdp raw;
    const RUdpTicket t = f.Ticket();
    for (int i = 0; i < 3; ++i)
    {
        SendHello(raw, f.ep(), t);
        const auto r = RecvReply(raw);
        CHECK(r && r->type == rudp::PacketType::Accept && r->conv == t.conv);
    }
    f.rec.WaitConnected(0);
    CHECK(f.rec.connected_count() == 1 && f.stats().accepted == 1 && f.stats().connections == 1);
}

// 同一来源已有会话时，另一个 conv 的 Hello 不理（不回、不消耗那张票据）。
void TestHelloWithOtherConvFromSameSourceIgnored()
{
    Fixture f;
    RawUdp raw;
    const RUdpTicket a = f.Ticket();
    const RUdpTicket b = f.Ticket();
    SendHello(raw, f.ep(), a);
    CHECK(RecvReply(raw)->type == rudp::PacketType::Accept);
    const auto dropped = f.stats().datagrams_dropped;
    SendHello(raw, f.ep(), b);
    CHECK(raw.ExpectSilence());
    CHECK(f.server->pending_tickets() == 1); // b 没被消耗
    CHECK(f.stats().datagrams_dropped == dropped + 1);
    CHECK(f.rec.connected_count() == 1);
}

void TestVersionMismatch()
{
    Fixture f;
    RawUdp raw;
    const RUdpTicket t = f.Ticket();
    auto hello = rudp::EncodeHello(t.conv, t.token);
    hello.bytes[1] = std::byte{rudp::kVersion + 1};
    raw.SendTo(f.ep(), hello.view());
    CHECK(IsReject(RecvReply(raw), rudp::RejectReason::Version));
    CHECK(f.server->pending_tickets() == 1); // 版本不对不消耗票据
    CHECK(f.rec.connected_count() == 0);
}

// 过滤器：在校验票据之前运行，拿到对端的 udp 端点；拒绝时回 Reject(Filtered)、计数、不消耗票据。
void TestAcceptFilter()
{
    std::mutex mu;
    std::optional<Endpoint> seen;
    std::optional<PeerCredentials> creds;
    std::atomic<bool> allow{false};
    RUdpServerOptions o;
    o.accept_filter = [&](const PeerInfo& p) noexcept
    {
        const std::lock_guard lk(mu);
        seen = p.endpoint;
        creds = p.credentials;
        return allow.load();
    };
    Fixture f(std::move(o));
    RawUdp raw;
    const RUdpTicket t = f.Ticket();
    SendHello(raw, f.ep(), t);
    CHECK(IsReject(RecvReply(raw), rudp::RejectReason::Filtered));
    {
        const std::lock_guard lk(mu);
        CHECK(seen.has_value() && *seen == raw.local() && seen->kind() == Endpoint::Kind::Udp);
        CHECK(!creds.has_value());
    }
    CHECK(f.stats().rejected_filter == 1 && f.stats().rejected_ticket == 0);
    CHECK(f.server->pending_tickets() == 1);
    allow = true;
    SendHello(raw, f.ep(), t);
    CHECK(RecvReply(raw)->type == rudp::PacketType::Accept);
}

// 连接表满：Reject(Busy)，不消耗票据；腾出位置后同一张票据还能用。
void TestConnectionTableFull()
{
    Fixture f(RUdpServerOptions{}, EventLoopGroupOptions{1, 2});
    RawUdp a, b, c;
    const RUdpTicket ta = f.Ticket(), tb = f.Ticket(), tc = f.Ticket();
    SendHello(a, f.ep(), ta);
    CHECK(RecvReply(a)->type == rudp::PacketType::Accept);
    SendHello(b, f.ep(), tb);
    CHECK(RecvReply(b)->type == rudp::PacketType::Accept);
    SendHello(c, f.ep(), tc);
    CHECK(IsReject(RecvReply(c), rudp::RejectReason::Busy));
    CHECK(f.stats().rejected_full == 1 && f.server->pending_tickets() == 1);

    a.SendTo(f.ep(), rudp::EncodeClose(ta.conv, 0).view()); // a 退出，腾出一个位置
    f.rec.WaitClosed(0);
    // 槽在 OnClosed 之后才回收：回收前来的 Hello 仍是 Busy，重试到成功
    bool accepted = false;
    for (int i = 0; i < 100 && !accepted; ++i)
    {
        SendHello(c, f.ep(), tc);
        const auto r = RecvReply(c);
        CHECK(r.has_value());
        accepted = r->type == rudp::PacketType::Accept;
        if (!accepted)
        {
            CHECK(IsReject(r, rudp::RejectReason::Busy));
            std::this_thread::sleep_for(20ms);
        }
    }
    CHECK(accepted);
}

// ---- 畸形包与陌生来源 ----

void TestMalformedDatagramsDropped()
{
    Fixture f;
    RawUdp raw;
    const std::uint64_t base = f.stats().datagrams_dropped;
    raw.SendTo(f.ep(), std::span<const std::byte>{});                                     // 空
    raw.SendTo(f.ep(), ToBytes("hello world, not our protocol at all"));                  // 垃圾
    raw.SendTo(f.ep(), Bytes{std::byte{0xA1}, std::byte{1}});                             // Hello 太短
    raw.SendTo(f.ep(), Bytes{std::byte{0xA9}, std::byte{1}, std::byte{2}, std::byte{3}}); // 类型不认识
    Bytes bad_len = {rudp::DataHeader()};
    bad_len.resize(10); // Data 不够一个 KCP 头
    raw.SendTo(f.ep(), bad_len);
    CHECK(raw.ExpectSilence());
    CHECK(WaitUntil([&] { return f.stats().datagrams_dropped == base + 5; }));
    // 服务端没被搞坏：正常握手
    const RUdpTicket t = f.Ticket();
    SendHello(raw, f.ep(), t);
    CHECK(RecvReply(raw)->type == rudp::PacketType::Accept);
}

// 没有会话的来源发 Data / Ping：回 Close(conv)，让重启前的客户端立刻知道会话没了；其它类型不理。
void TestUnknownSourceGetsClose()
{
    Fixture f;
    RawUdp raw;
    raw.SendTo(f.ep(), rudp::EncodePing(0x55u).view());
    auto r = RecvReply(raw);
    CHECK(r && r->type == rudp::PacketType::Close && r->conv == 0x55u);

    Bytes data(rudp::kDataPrefix + rudp::kKcpHeader, std::byte{0});
    data[0] = rudp::DataHeader();
    data[1] = std::byte{0x04};
    data[2] = std::byte{0x03};
    data[3] = std::byte{0x02};
    data[4] = std::byte{0x01};
    raw.SendTo(f.ep(), data);
    r = RecvReply(raw);
    CHECK(r && r->type == rudp::PacketType::Close && r->conv == 0x01020304u);

    raw.SendTo(f.ep(), rudp::EncodePong(1).view());
    raw.SendTo(f.ep(), rudp::EncodeClose(1, 0).view());
    raw.SendTo(f.ep(), rudp::EncodeAccept(1).view());
    raw.SendTo(f.ep(), rudp::EncodeReject(rudp::RejectReason::Busy).view());
    CHECK(raw.ExpectSilence());
    CHECK(f.stats().datagrams_dropped == 6);
}

// 按来源固定：另一个来源即使知道 conv，也读不到、写不进、关不掉别人的会话。
void TestAddressPinning()
{
    Fixture f;
    RawUdp owner_sock, intruder_sock;
    const RUdpTicket t = f.Ticket();
    KcpPeer real(owner_sock, f.ep(), t.conv);
    CHECK(real.Handshake(t));
    f.rec.WaitConnected(0);

    KcpPeer intruder(intruder_sock, f.ep(), t.conv); // 用同一个 conv
    intruder.Send("injected");
    intruder.Run(150ms);
    CHECK(f.rec.message_count() == 0);                           // 消息没进去
    CHECK(intruder.CountControls(rudp::PacketType::Close) >= 1); // 它只会得到 Close
    intruder_sock.SendTo(f.ep(), rudp::EncodeClose(t.conv, 0).view());
    intruder_sock.SendTo(f.ep(), rudp::EncodePing(t.conv).view());
    std::this_thread::sleep_for(100ms);
    CHECK(f.rec.closed_count() == 0); // 关不掉

    real.Send("legit");
    CHECK(real.RunUntil([&] { return f.rec.message_count() >= 1; })); // KcpPeer 要 Run 才推进 KCP
    CHECK(f.rec.payloads()[0] == "legit");
}

// ---- 心跳与 Close ----

void TestPingPong()
{
    Fixture f;
    RawUdp raw;
    const RUdpTicket t = f.Ticket();
    SendHello(raw, f.ep(), t);
    CHECK(RecvReply(raw)->type == rudp::PacketType::Accept);
    raw.SendTo(f.ep(), rudp::EncodePing(t.conv).view());
    const auto r = RecvReply(raw);
    CHECK(r && r->type == rudp::PacketType::Pong && r->conv == t.conv);
    // conv 不对的 Ping（来源对）：不回，计入丢弃
    const auto dropped = f.stats().datagrams_dropped;
    raw.SendTo(f.ep(), rudp::EncodePing(t.conv + 1).view());
    CHECK(raw.ExpectSilence());
    CHECK(f.stats().datagrams_dropped == dropped + 1);
}

// 对端发来 Close：以 PeerClosed 结束，不回 Close；conv 不对的 Close 不理。
void TestPeerClose()
{
    Fixture f;
    RawUdp raw;
    const RUdpTicket t = f.Ticket();
    SendHello(raw, f.ep(), t);
    CHECK(RecvReply(raw)->type == rudp::PacketType::Accept);
    f.rec.WaitConnected(0);
    raw.SendTo(f.ep(), rudp::EncodeClose(t.conv + 1, 0).view());
    std::this_thread::sleep_for(100ms);
    CHECK(f.rec.closed_count() == 0);
    raw.SendTo(f.ep(), rudp::EncodeClose(t.conv, 0).view());
    const auto c = f.rec.WaitClosed(0);
    CHECK(c.reason == CloseReason::PeerClosed);
    CHECK(raw.ExpectSilence()); // 对端发的 Close 不回
    CHECK(f.stats().connections == 0);
    f.rec.CheckInvariants();
}

// 本端关闭：向对端发 3 个 Close（reason = LocalClose）；之后该来源再发包被当陌生来源。
void TestLocalCloseSendsCloseBurst()
{
    Fixture f;
    RawUdp raw;
    const RUdpTicket t = f.Ticket();
    SendHello(raw, f.ep(), t);
    CHECK(RecvReply(raw)->type == rudp::PacketType::Accept);
    const ConnectionHandle h = f.rec.WaitConnected(0);
    h.Close();
    int closes = 0;
    while (const auto r = RecvReply(raw, 500ms))
    {
        CHECK(r->type == rudp::PacketType::Close && r->conv == t.conv);
        CHECK(r->reason == static_cast<std::uint8_t>(CloseReason::LocalClose));
        ++closes;
    }
    CHECK(closes == 3);
    CHECK(f.rec.WaitClosed(0).reason == CloseReason::LocalClose);
    // 会话已回收：这个来源再发 Ping 当陌生来源处理
    raw.SendTo(f.ep(), rudp::EncodePing(t.conv).view());
    const auto r = RecvReply(raw);
    CHECK(r && r->type == rudp::PacketType::Close);
}

// 空闲：开了 idle_timeout，来源一直不吭声就以 IdleTimeout 关闭；定期发心跳的不会被关。
void TestIdleTimeout()
{
    RUdpServerOptions o;
    o.connection.idle_timeout = 600ms;
    Fixture f(std::move(o));
    RawUdp quiet, chatty;
    const RUdpTicket tq = f.Ticket(), tc = f.Ticket();
    SendHello(quiet, f.ep(), tq);
    CHECK(RecvReply(quiet)->type == rudp::PacketType::Accept);
    SendHello(chatty, f.ep(), tc);
    CHECK(RecvReply(chatty)->type == rudp::PacketType::Accept);
    const ConnectionHandle hq = f.rec.WaitConnected(0);
    f.rec.WaitConnected(1);

    const auto until = std::chrono::steady_clock::now() + 2500ms;
    while (std::chrono::steady_clock::now() < until)
    {
        chatty.SendTo(f.ep(), rudp::EncodePing(tc.conv).view());
        std::this_thread::sleep_for(100ms);
        chatty.Drain();
    }
    CHECK(f.rec.closed_count() == 1);
    const auto closed = f.rec.WaitClosed(0);
    CHECK(closed.reason == CloseReason::IdleTimeout);
    CHECK(closed.handle == hq); // 被关的是 quiet（先握手的）
    CHECK(f.stats().closed[static_cast<std::size_t>(CloseReason::IdleTimeout)] == 1);
}

// ---- Stop / 析构 / 启动 ----

// Stop：不再接受新会话（Hello 得到 Reject(Stopping)，不消耗票据），已有会话继续；最后一个会话结束后端口释放。
void TestStopRejectsNewButKeepsExisting()
{
    Fixture f;
    RawUdp live, late;
    const RUdpTicket t1 = f.Ticket(), t2 = f.Ticket();
    SendHello(live, f.ep(), t1);
    CHECK(RecvReply(live)->type == rudp::PacketType::Accept);
    f.rec.WaitConnected(0);

    const Endpoint ep = f.ep();
    f.server->Stop();
    f.Barrier();
    SendHello(late, ep, t2);
    CHECK(IsReject(RecvReply(late), rudp::RejectReason::Stopping));
    CHECK(f.server->pending_tickets() == 1);

    live.SendTo(ep, rudp::EncodePing(t1.conv).view()); // 已有会话照常
    CHECK(RecvReply(live)->type == rudp::PacketType::Pong);

    // 端口还被占着（有会话）；会话结束后释放
    live.SendTo(ep, rudp::EncodeClose(t1.conv, 0).view());
    f.rec.WaitClosed(0);
    f.Barrier();
    // 能重新绑定同一端口 = 端口已释放
    sockaddr_storage ss;
    socklen_t len = sock::ToSockaddr(ep, ss);
    UniqueFd fd;
    CHECK(sock::CreateSocket(ep, SOCK_DGRAM, fd) == 0);
    CHECK(WaitUntil([&] { return ::bind(fd.get(), reinterpret_cast<const sockaddr*>(&ss), len) == 0; }, 3s));
}

// Stop 时没有会话：端口立刻释放。对没 Start 过的对象是空操作。
void TestStopWithoutSessionsReleasesPort()
{
    Fixture f;
    const Endpoint ep = f.ep();
    f.server->Stop();
    f.Barrier();
    sockaddr_storage ss;
    socklen_t len = sock::ToSockaddr(ep, ss);
    UniqueFd fd;
    CHECK(sock::CreateSocket(ep, SOCK_DGRAM, fd) == 0);
    CHECK(::bind(fd.get(), reinterpret_cast<const sockaddr*>(&ss), len) == 0);

    EventLoopGroup g2;
    CHECK(g2.Start() == 0);
    EventRecorder rec2;
    RUdpServer never_started(g2, RUdpLoopback(), RUdpServerOptions{}, rec2);
    never_started.Stop(); // 空操作
    g2.Shutdown(1s);
}

void TestStartErrors()
{
    EventLoopGroup group(EventLoopGroupOptions{1, 64});
    CHECK(group.Start() == 0);
    EventRecorder rec;
    {
        RUdpServerOptions bad;
        bad.rudp.mtu = 100; // 低于下限
        RUdpServer s(group, RUdpLoopback(), std::move(bad), rec);
        CHECK(s.Start() == EINVAL);
    }
    {
        RUdpServerOptions bad;
        bad.rudp.max_message_size = rudp::MaxMessageForMtu(1400) + 1;
        RUdpServer s(group, RUdpLoopback(), std::move(bad), rec);
        CHECK(s.Start() == EINVAL);
    }
    RUdpServer first(group, RUdpLoopback(), RUdpServerOptions{}, rec);
    CHECK(first.Start() == 0);
    {
        RUdpServer second(group, *RUdpEndpoint::From(first.endpoint()), RUdpServerOptions{}, rec);
        CHECK(second.Start() == EADDRINUSE);
    }
    // 失败的启动不影响第一个
    RawUdp raw;
    const auto t = first.IssueTicket(10s);
    raw.SendTo(first.endpoint(), rudp::EncodeHello(t->conv, t->token).view());
    CHECK(RecvReply(raw)->type == rudp::PacketType::Accept);
    group.Shutdown(2s);
    RUdpServer after(group, RUdpLoopback(), RUdpServerOptions{}, rec);
    CHECK(after.Start() == ESHUTDOWN);
}

void TestEndpointBeforeAndAfterStart()
{
    EventLoopGroup group(EventLoopGroupOptions{1, 64});
    CHECK(group.Start() == 0);
    EventRecorder rec;
    RUdpServer s(group, RUdpLoopback(0), RUdpServerOptions{}, rec);
    CHECK(s.endpoint().port() == 0 && s.local_endpoint().port() == 0);
    CHECK(s.Start() == 0);
    CHECK(s.endpoint().port() != 0 && s.endpoint().kind() == Endpoint::Kind::Udp);
    CHECK(s.local_endpoint().endpoint() == s.endpoint());
    group.Shutdown(1s);
}

// 析构时 group 还在跑、会话还活着：先同步关掉全部会话并等它们回收（OnClosed 都已回调），不崩、不泄漏。
void TestDestructorWithLiveSessions()
{
    LeakCheck leak;
    EventLoopGroup group(EventLoopGroupOptions{2, 64});
    CHECK(group.Start() == 0);
    EventRecorder rec;
    std::vector<std::unique_ptr<RawUdp>> raws;
    auto dtor_start = std::chrono::steady_clock::now();
    {
        RUdpServer server(group, RUdpLoopback(), RUdpServerOptions{}, rec);
        CHECK(server.Start() == 0);
        for (int i = 0; i < 5; ++i)
        {
            raws.push_back(std::make_unique<RawUdp>());
            const auto t = server.IssueTicket(10s);
            raws.back()->SendTo(server.endpoint(), rudp::EncodeHello(t->conv, t->token).view());
            CHECK(RecvReply(*raws.back())->type == rudp::PacketType::Accept);
        }
        CHECK(rec.connected_count() == 5);
        dtor_start = std::chrono::steady_clock::now();
    } // 析构
    CHECK(std::chrono::steady_clock::now() - dtor_start < 2s); // 不靠"等 5 秒超时"才返回
    CHECK(rec.closed_count() == 5);                            // 返回时已全部回收
    for (std::size_t i = 0; i < 5; ++i)
        CHECK(rec.WaitClosed(i).reason == CloseReason::LocalClose);
    rec.CheckInvariants();
    CHECK(group.GetStats().connections == 0);
    for (auto& r : raws) // 每个对端都收到了 Close
    {
        bool got_close = false;
        while (const auto m = RecvReply(*r, 300ms))
            got_close = got_close || m->type == rudp::PacketType::Close;
        CHECK(got_close);
    }
    group.Shutdown(1s);
}

// group 先 Shutdown（优雅关闭所有会话），服务端对象之后才销毁。
void TestGroupShutdownClosesSessions()
{
    LeakCheck leak;
    EventLoopGroup group(EventLoopGroupOptions{2, 64});
    CHECK(group.Start() == 0);
    EventRecorder rec;
    RUdpServer server(group, RUdpLoopback(), RUdpServerOptions{}, rec);
    CHECK(server.Start() == 0);
    RawUdp raw;
    const auto t = server.IssueTicket(10s);
    raw.SendTo(server.endpoint(), rudp::EncodeHello(t->conv, t->token).view());
    CHECK(RecvReply(raw)->type == rudp::PacketType::Accept);
    rec.WaitConnected(0);
    group.Shutdown(2s);
    CHECK(rec.closed_count() == 1 && rec.WaitClosed(0).reason == CloseReason::LocalClose);
    rec.CheckInvariants();
}

// 许多会话并发握手、各自心跳：按来源分得开，计数对得上。
void TestManySessions()
{
    Fixture f(RUdpServerOptions{}, EventLoopGroupOptions{2, 512});
    constexpr int kN = 100;
    std::vector<std::unique_ptr<RawUdp>> raws;
    std::vector<RUdpTicket> tickets;
    for (int i = 0; i < kN; ++i)
    {
        raws.push_back(std::make_unique<RawUdp>());
        tickets.push_back(f.Ticket());
        SendHello(*raws[static_cast<std::size_t>(i)], f.ep(), tickets.back());
    }
    for (int i = 0; i < kN; ++i)
    {
        const auto r = RecvReply(*raws[static_cast<std::size_t>(i)]);
        CHECK(r && r->type == rudp::PacketType::Accept && r->conv == tickets[static_cast<std::size_t>(i)].conv);
    }
    CHECK(WaitUntil([&] { return f.rec.connected_count() == kN; }));
    CHECK(f.stats().accepted == kN && f.stats().connections == kN);
    for (int i = 0; i < kN; ++i)
        raws[static_cast<std::size_t>(i)]->SendTo(f.ep(),
                                                  rudp::EncodePing(tickets[static_cast<std::size_t>(i)].conv).view());
    for (int i = 0; i < kN; ++i)
    {
        const auto r = RecvReply(*raws[static_cast<std::size_t>(i)]);
        CHECK(r && r->type == rudp::PacketType::Pong && r->conv == tickets[static_cast<std::size_t>(i)].conv);
    }
}

// 在途的数据不用每拍都调 Update：服务端在 OnConnected 里给 40 个会话各发一条，对端（裸 socket）一直不确认。
// 只有到了重传时刻才需要 flush：600ms 里每个会话该有几次重传（间隔 200ms 起、按 1.5 倍退避），Update 的次数
// 只有每拍都调（40 × 60）的零头；但重传一次都不能少——调度没睡过头。
void TestInFlightSessionsAreOnlyUpdatedWhenDue()
{
    Fixture f;
    f.rec.on_connected = [](IConnection& c) { CHECK(c.Send(std::as_bytes(std::span("hi", 2))) == SendResult::Ok); };
    constexpr int kN = 40;
    std::vector<std::unique_ptr<RawUdp>> raws;
    for (int i = 0; i < kN; ++i)
    {
        raws.push_back(std::make_unique<RawUdp>());
        SendHello(*raws.back(), f.ep(), f.Ticket());
        CHECK(RecvReply(*raws.back())->type == rudp::PacketType::Accept);
    }
    CHECK(WaitUntil([&] { return f.rec.connected_count() == kN; }));
    std::this_thread::sleep_for(100ms); // 第一次 flush 已经发出去
    const std::uint64_t before = f.stats().rudp_updates;
    std::this_thread::sleep_for(600ms);
    const std::uint64_t updates = f.stats().rudp_updates - before;
    CHECK(updates >= kN);         // 每个会话至少有一次重传到点（计数没有漏记）
    CHECK(updates < kN * 60 / 4); // 每拍都调是 2400 次；到点才调只有零头
    for (auto& raw : raws)        // 每个对端收到：第一次发出 + 至少一次重传
    {
        std::size_t datas = 0;
        while (const auto dg = raw->Recv(0ms))
        {
            const auto pk = rudp::Parse(*dg);
            datas += pk && pk->type == rudp::PacketType::Data ? 1 : 0;
        }
        CHECK(datas >= 2);
    }
}

// 睡着等重传的会话被新的 Send 唤醒后，下一拍就 flush，不用等到重传时刻：
// 服务端先发一条没人确认的消息（在途，睡到 RTO），过一会儿业务再发一条，
// 它必须在几个 interval 内出现在对端，而不是等 200ms 之后的重传把它带出去。
void TestSendWhileInFlightIsFlushedPromptly()
{
    Fixture f;
    f.rec.on_connected = [](IConnection& c) { CHECK(c.Send(std::as_bytes(std::span("first", 5))) == SendResult::Ok); };
    RawUdp raw;
    const RUdpTicket t = f.Ticket();
    SendHello(raw, f.ep(), t);
    CHECK(RecvReply(raw)->type == rudp::PacketType::Accept);
    const ConnectionHandle h = f.rec.WaitConnected(0);
    // 第一条发出（对端是裸 socket，不确认）：收下并确认它是 Data
    const auto first = raw.Recv(1s);
    CHECK(first.has_value() && rudp::Parse(*first)->type == rudp::PacketType::Data);
    std::this_thread::sleep_for(60ms); // 会话睡到第一条的重传时刻（约 200ms）
    const auto sent_at = std::chrono::steady_clock::now();
    CHECK(h.Send(Buf("second")) == SendResult::Ok);
    bool seen = false;
    while (std::chrono::steady_clock::now() - sent_at < 100ms && !seen) // 远早于第一条的重传
    {
        const auto dg = raw.Recv(20ms);
        if (!dg)
            continue;
        const auto pk = rudp::Parse(*dg);
        // 数据报里的段带着 "second"（KCP 头 24 字节之后就是消息内容）
        if (pk && pk->type == rudp::PacketType::Data && pk->body.size() >= rudp::kKcpHeader + 6)
            seen = Str(pk->body.subspan(rudp::kKcpHeader)).find("second") != std::string::npos;
    }
    CHECK(seen);
}

// 静止了一阵的会话（KCP 的时钟停在握手时）再发一条并立刻 flush（flush_on_send）：重传时刻要按"现在"算。
// 时钟没拨的话，重传时刻是 RTO 之前的"过去"，下一拍就多发一个提前的重传；对端只该看到一个数据报，
// 下一个要等 200ms 之后的真正重传。
void TestSendAfterIdleIsNotRetransmittedPrematurely()
{
    RUdpServerOptions o;
    o.rudp.flush_on_send = true;
    Fixture f(std::move(o));
    RawUdp raw;
    const RUdpTicket t = f.Ticket();
    SendHello(raw, f.ep(), t);
    CHECK(RecvReply(raw)->type == rudp::PacketType::Accept);
    const ConnectionHandle h = f.rec.WaitConnected(0);
    std::this_thread::sleep_for(700ms); // 会话静止，不会有人去 Update 它
    raw.Drain();
    CHECK(h.Send(Buf("ping")) == SendResult::Ok);
    int datas = 0;
    const auto until = std::chrono::steady_clock::now() + 120ms; // 远短于第一次重传（约 200ms）
    while (std::chrono::steady_clock::now() < until)
    {
        const auto dg = raw.Recv(20ms);
        if (!dg)
            continue;
        const auto pk = rudp::Parse(*dg);
        datas += pk && pk->type == rudp::PacketType::Data ? 1 : 0;
    }
    CHECK(datas == 1);
}

// 已建立会话的来源发来 conv 不对的数据：KCP 不认，丢弃并计数，消息不会交付，会话不受影响。
void TestWrongConvDataFromPinnedSourceDropped()
{
    Fixture f;
    RawUdp raw;
    const RUdpTicket t = f.Ticket();
    KcpPeer real(raw, f.ep(), t.conv);
    CHECK(real.Handshake(t));
    f.rec.WaitConnected(0);
    const auto dropped = f.stats().datagrams_dropped;
    KcpPeer wrong(raw, f.ep(), t.conv + 1); // 同一个来源地址，conv 不对
    wrong.Send("poison");
    wrong.Run(150ms);
    CHECK(f.stats().datagrams_dropped > dropped);
    CHECK(f.rec.message_count() == 0 && f.rec.closed_count() == 0);
    real.Send("fine");
    CHECK(real.RunUntil([&] { return f.rec.message_count() == 1; }));
}

// 只发数据、不发心跳也算活跃：每 100ms 一条消息，idle_timeout 600ms 下撑过 2 秒不被关。
void TestDataKeepsSessionAlive()
{
    RUdpServerOptions o;
    o.connection.idle_timeout = 600ms;
    Fixture f(std::move(o));
    RawUdp raw;
    const RUdpTicket t = f.Ticket();
    KcpPeer peer(raw, f.ep(), t.conv);
    CHECK(peer.Handshake(t));
    f.rec.WaitConnected(0);
    const auto until = std::chrono::steady_clock::now() + 2s;
    int n = 0;
    while (std::chrono::steady_clock::now() < until)
    {
        peer.Send("beat" + std::to_string(n++));
        peer.Run(100ms);
    }
    CHECK(f.rec.closed_count() == 0);
    CHECK(f.rec.message_count() >= 15);
}

// Accept 先于 OnConnected 里发的数据到达：服务端在 OnConnected 里就 Send（flush_on_send，数据立刻出去），
// 客户端收到的第一个数据报必须是 Accept，否则数据会撞上"还在握手"被丢，白等一个重传。
void TestAcceptPrecedesDataSentFromOnConnected()
{
    RUdpServerOptions o;
    o.rudp.flush_on_send = true;
    Fixture f(std::move(o));
    f.rec.on_connected = [](IConnection& c) { CHECK(c.Send(std::as_bytes(std::span("hello", 5))) == SendResult::Ok); };
    RawUdp raw;
    const RUdpTicket t = f.Ticket();
    SendHello(raw, f.ep(), t);
    const auto first = RecvReply(raw);
    CHECK(first && first->type == rudp::PacketType::Accept);
    const auto second = RecvReply(raw);
    CHECK(second && second->type == rudp::PacketType::Data);
}

// 摘表是 swap-remove：中间的会话关掉之后，被挪动的会话仍然按自己的来源找得到、收得到自己的心跳回应。
void TestSessionsSurviveSwapRemove()
{
    Fixture f;
    constexpr int kN = 6;
    std::vector<std::unique_ptr<RawUdp>> raws;
    std::vector<RUdpTicket> tickets;
    for (int i = 0; i < kN; ++i)
    {
        raws.push_back(std::make_unique<RawUdp>());
        tickets.push_back(f.Ticket());
        SendHello(*raws.back(), f.ep(), tickets.back());
        CHECK(RecvReply(*raws.back())->type == rudp::PacketType::Accept);
    }
    // 关掉第 0、2、3 个（头部与中间），剩下的被换位
    for (const int i : {0, 2, 3})
        raws[static_cast<std::size_t>(i)]->SendTo(
            f.ep(), rudp::EncodeClose(tickets[static_cast<std::size_t>(i)].conv, 0).view());
    CHECK(WaitUntil([&] { return f.rec.closed_count() == 3; }));
    for (const int i : {1, 4, 5})
    {
        auto& raw = *raws[static_cast<std::size_t>(i)];
        const auto conv = tickets[static_cast<std::size_t>(i)].conv;
        raw.Drain();
        raw.SendTo(f.ep(), rudp::EncodePing(conv).view());
        const auto r = RecvReply(raw);
        CHECK(r && r->type == rudp::PacketType::Pong && r->conv == conv);
    }
    // 再关一个被换过位的，其余照样正常
    raws[5]->SendTo(f.ep(), rudp::EncodeClose(tickets[5].conv, 0).view());
    CHECK(WaitUntil([&] { return f.rec.closed_count() == 4; }));
    for (const int i : {1, 4})
    {
        auto& raw = *raws[static_cast<std::size_t>(i)];
        raw.Drain();
        raw.SendTo(f.ep(), rudp::EncodePing(tickets[static_cast<std::size_t>(i)].conv).view());
        CHECK(RecvReply(raw)->type == rudp::PacketType::Pong);
    }
}

// KCP 的内部时钟靠服务端的定时器推进：10ms 一拍下，20 个来回的回显在 2 秒内做完（定时器慢了就是几十秒）。
void TestTickIntervalDrivesLatency()
{
    Fixture f;
    f.rec.on_message = [](IConnection& c, std::string_view sv) { Echo(c, sv); };
    RawUdp raw;
    const RUdpTicket t = f.Ticket();
    KcpPeer peer(raw, f.ep(), t.conv);
    CHECK(peer.Handshake(t));
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i)
    {
        peer.Send("rt" + std::to_string(i));
        CHECK(peer.RunUntil([&] { return peer.messages().size() == static_cast<std::size_t>(i) + 1; }, 5s));
    }
    CHECK(std::chrono::steady_clock::now() - start < 2s);
}

// loop 已经跑过的轮数（所有 loop 加起来）：空转的话它会按定时器的节拍一直涨。
std::uint64_t LoopIterations(const EventLoopGroup& g)
{
    std::uint64_t n = 0;
    for (std::size_t i = 0; i < GroupAccess::LoopCount(g); ++i)
        n += GroupAccess::LoopAt(g, i).Iterations();
    return n;
}

// 空闲的会话不占定时器：建好 30 个会话、什么都不发，loop 不应该每 10ms 醒一次去走一遍它们。
void TestIdleSessionsDoNotSpinTheLoop()
{
    Fixture f;
    std::vector<std::unique_ptr<RawUdp>> raws;
    for (int i = 0; i < 30; ++i)
    {
        raws.push_back(std::make_unique<RawUdp>());
        SendHello(*raws.back(), f.ep(), f.Ticket());
        CHECK(RecvReply(*raws.back())->type == rudp::PacketType::Accept);
    }
    CHECK(WaitUntil([&] { return f.rec.connected_count() == 30; }));
    std::this_thread::sleep_for(150ms);
    const std::uint64_t before = LoopIterations(f.group);
    std::this_thread::sleep_for(600ms);
    CHECK(LoopIterations(f.group) - before < 15); // 以前每 10ms 一拍：这里会有约 60 轮
}

// 活动结束后会话回到静止：来回收发几条之后（要靠定时器 flush 与确认），定时器不再空转。
void TestSessionGoesQuietAfterActivity()
{
    Fixture f;
    f.rec.on_message = [](IConnection& c, std::string_view sv) { Echo(c, sv); };
    RawUdp raw;
    const RUdpTicket t = f.Ticket();
    KcpPeer peer(raw, f.ep(), t.conv);
    CHECK(peer.Handshake(t));
    for (int i = 0; i < 5; ++i)
    {
        peer.Send("q" + std::to_string(i));
        CHECK(peer.RunUntil([&] { return peer.messages().size() == static_cast<std::size_t>(i) + 1; }));
    }
    peer.Run(150ms);                    // 假对端要继续跑：它对最后一条回显的确认要靠下一次 Update 才发出去
    std::this_thread::sleep_for(300ms); // 让服务端处理完最后的确认、退出活跃表
    const std::uint64_t before = LoopIterations(f.group);
    std::this_thread::sleep_for(600ms);
    CHECK(LoopIterations(f.group) - before < 15);
    // 再来一轮仍然正常（静止的会话被数据唤醒）
    peer.Send("again");
    CHECK(peer.RunUntil([&] { return peer.messages().size() == 6; }));
}

// 收到的数据要被及时确认，哪怕服务端没有任何东西要发：静止的会话被数据唤醒、下一拍 flush 确认。
void TestDataIsAckedPromptly()
{
    Fixture f; // 默认的批量配置：确认只能靠定时器发
    RawUdp raw;
    const RUdpTicket t = f.Ticket();
    KcpPeer peer(raw, f.ep(), t.conv);
    CHECK(peer.Handshake(t));
    const auto start = std::chrono::steady_clock::now();
    peer.Send("ack me");
    CHECK(peer.RunUntil([&] { return peer.kcp().WaitSnd() == 0; }, 1s));
    CHECK(std::chrono::steady_clock::now() - start < 300ms); // 几个 interval 之内，不是等 RTO 重传
    CHECK(f.rec.message_count() == 1);
}

// 会话在"还在活跃表里"时被关闭（对端的 Close）：摘表时要一并摘出活跃表，剩下的会话照常被推进。
// 6 个会话里 0、2、3 号收了数据却从不确认（一直在重传、一直活跃），按 2、0、3 的顺序关掉，1、4、5 号照常回显。
void TestSessionsClosedWhileTickingLeaveTheRestWorking()
{
    Fixture f;
    f.rec.on_connected = [](IConnection& c) { CHECK(c.Send(std::as_bytes(std::span("hi", 2))) == SendResult::Ok); };
    f.rec.on_message = [](IConnection& c, std::string_view sv) { Echo(c, sv); };
    constexpr int kN = 6;
    std::vector<std::unique_ptr<RawUdp>> raws;
    std::vector<std::unique_ptr<KcpPeer>> peers;
    std::vector<RUdpTicket> tickets;
    for (int i = 0; i < kN; ++i)
    {
        raws.push_back(std::make_unique<RawUdp>());
        tickets.push_back(f.Ticket());
        peers.push_back(std::make_unique<KcpPeer>(*raws.back(), f.ep(), tickets.back().conv));
        CHECK(peers.back()->Handshake(tickets.back()));
    }
    const std::set<int> silent = {0, 2, 3};
    for (int i = 0; i < kN; ++i) // 活跃的收下 "hi" 并确认；沉默的什么都不回
        if (!silent.count(i))
            CHECK(peers[static_cast<std::size_t>(i)]->RunUntil(
                [&] { return peers[static_cast<std::size_t>(i)]->messages().size() == 1; }));
    std::this_thread::sleep_for(100ms); // 沉默的几个在重传，处于活跃表里
    for (const int i : {2, 0, 3})
        raws[static_cast<std::size_t>(i)]->SendTo(
            f.ep(), rudp::EncodeClose(tickets[static_cast<std::size_t>(i)].conv, 0).view());
    CHECK(WaitUntil([&] { return f.rec.closed_count() == 3; }));
    for (const int i : {1, 4, 5})
    {
        auto& p = *peers[static_cast<std::size_t>(i)];
        p.Send("alive" + std::to_string(i));
        CHECK(p.RunUntil([&] { return p.messages().size() == 2; }));
        CHECK(p.messages()[1] == "alive" + std::to_string(i));
    }
}

// 会话的数据通路（用 KcpPeer 讲真实 KCP）：消息边界与顺序、对端的超限消息按协议错误关闭。
void TestDataPathAndOversizeFromPeer()
{
    RUdpServerOptions o;
    o.rudp.max_message_size = 1000;
    Fixture f(std::move(o));
    f.rec.on_message = [](IConnection& c, std::string_view sv) { Echo(c, sv); };
    RawUdp raw;
    const RUdpTicket t = f.Ticket();
    KcpPeer peer(raw, f.ep(), t.conv);
    CHECK(peer.Handshake(t));
    peer.Send("one");
    peer.Send("");
    peer.Send(std::string(900, 'x'));
    CHECK(peer.RunUntil([&] { return peer.messages().size() == 3; }));
    CHECK(peer.messages()[0] == "one" && peer.messages()[1].empty() && peer.messages()[2] == std::string(900, 'x'));
    CHECK(f.rec.message_count() == 3);

    peer.Send(std::string(1001, 'y')); // 超过服务端的 max_message_size
    CHECK(peer.RunUntil([&] { return f.rec.closed_count() >= 1; }));
    const auto c = f.rec.WaitClosed(0);
    CHECK(c.reason == CloseReason::ProtocolError);
    CHECK(peer.RunUntil([&] { return peer.CountControls(rudp::PacketType::Close) >= 1; }));
    CHECK(f.stats().messages_in == 3);
}

} // namespace

int main()
{
    RUN(TestHandshakeAccept);
    RUN(TestUnknownTicketRejected);
    RUN(TestWrongTokenRejectedThenRightOne);
    RUN(TestTicketSingleUse);
    RUN(TestTicketExpired);
    RUN(TestAddTicket);
    RUN(TestTicketLimits);
    RUN(TestDuplicateHelloResendsAccept);
    RUN(TestHelloWithOtherConvFromSameSourceIgnored);
    RUN(TestVersionMismatch);
    RUN(TestAcceptFilter);
    RUN(TestConnectionTableFull);
    RUN(TestMalformedDatagramsDropped);
    RUN(TestUnknownSourceGetsClose);
    RUN(TestAddressPinning);
    RUN(TestPingPong);
    RUN(TestPeerClose);
    RUN(TestLocalCloseSendsCloseBurst);
    RUN(TestIdleTimeout);
    RUN(TestStopRejectsNewButKeepsExisting);
    RUN(TestStopWithoutSessionsReleasesPort);
    RUN(TestStartErrors);
    RUN(TestEndpointBeforeAndAfterStart);
    RUN(TestDestructorWithLiveSessions);
    RUN(TestGroupShutdownClosesSessions);
    RUN(TestManySessions);
    RUN(TestInFlightSessionsAreOnlyUpdatedWhenDue);
    RUN(TestSendWhileInFlightIsFlushedPromptly);
    RUN(TestSendAfterIdleIsNotRetransmittedPrematurely);
    RUN(TestWrongConvDataFromPinnedSourceDropped);
    RUN(TestDataKeepsSessionAlive);
    RUN(TestAcceptPrecedesDataSentFromOnConnected);
    RUN(TestSessionsSurviveSwapRemove);
    RUN(TestTickIntervalDrivesLatency);
    RUN(TestIdleSessionsDoNotSpinTheLoop);
    RUN(TestSessionGoesQuietAfterActivity);
    RUN(TestDataIsAckedPromptly);
    RUN(TestSessionsClosedWhileTickingLeaveTheRestWorking);
    RUN(TestDataPathAndOversizeFromPeer);
    std::puts("rudp_server_test: ok");
    return 0;
}
