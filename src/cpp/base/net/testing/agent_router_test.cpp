// Agent 的路由器：解析信封、核对身份、判过期、查路由表、投递（共享内存的 Rx 环 / 仅 UDS 的连接 / 别的 Agent 的链路）。
// 设计见 docs/共享内存IPC设计.md §4 / §10，约定见 agent_router.h。
//
// 无测试框架依赖：失败即 abort，并发用例带超时。

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "base/net/agent/agent_router.h"
#include "base/net/agent/route_table.h"
#include "base/net/tcp/tcp_client.h"
#include "base/net/tcp/tcp_server.h"
#include "base/net/testing/shm_channel_test_util.h"

using namespace shmtest;
using namespace breeze::agent;

namespace
{

constexpr NodeId kA = 101, kB = 102, kC = 103, kD = 104;

std::uint64_t SteadyNowNs()
{
    return static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()); // 与 Now::SteadyNs（steady_clock）同一个时钟
}

ShmServerOptions Ret()
{
    ShmServerOptions o;
    o.retain_default = 30s;
    return o;
}

// 一个 Agent：路由表 + 路由器 + 服务端（共享内存与仅 UDS 都支持）。io_threads = 1：路由器单线程。
struct AgentEnv
{
    TempDir dir;
    StaticRouteTable table;
    std::atomic<std::int64_t> clock_offset_ns{0}; // 拨快路由器的时钟，模拟"排了很久的队"
    AgentRouter router;
    LengthPrefixCodecFactory codec;
    EventLoopGroup group;
    Endpoint ep;
    std::unique_ptr<ShmChannelServer> server;

    explicit AgentEnv(AgentRouterOptions ro = {}, ShmServerOptions so = Ret())
        : router(table, WithClock(std::move(ro)))
        , codec(1 << 20)
        , group(EventLoopGroupOptions{1, 256})
        , ep(dir.Unix("agent.sock"))
    {
        CHECK(group.Start() == 0);
        ShmUdsDataPath path;
        path.handler = &router;
        path.factory = &codec;
        server = std::make_unique<ShmChannelServer>(group, ep, std::move(so), router, path);
        CHECK(server->Start() == 0);
    }

    AgentRouterOptions WithClock(AgentRouterOptions ro)
    {
        ro.clock = [this] { return static_cast<std::uint64_t>(static_cast<std::int64_t>(SteadyNowNs()) + clock_offset_ns.load()); };
        return ro;
    }
    void SetClockOffset(std::chrono::milliseconds ms) { clock_offset_ns.store(ms.count() * 1'000'000); }

    // 等路由器的统计满足条件（计数在回调里更新，晚于收发）。
    template <typename Pred>
    bool WaitStats(Pred pred, std::chrono::milliseconds timeout = 5s)
    {
        return WaitUntil([&] { return pred(router.stats()); }, timeout);
    }
};

// ---- 消息 ----

struct Msg
{
    EnvelopeHead head{};
    std::string body;
};

struct Spec
{
    EnvType type = EnvType::kOneway;
    RouteType route = RouteType::kDirect;
    NodeId src = 0;
    NodeId dst = 0;
    std::uint64_t seq = 1;
    std::uint32_t remaining_ms = 5000;
    std::uint16_t service = 0;
    std::uint64_t key = 0;
    std::uint32_t msg_id = 7;
    std::uint8_t hops = 0;
    std::uint32_t err = 0;
};

std::vector<std::byte> Build(const Spec& s, std::string_view body = "payload")
{
    EnvelopeHead h{};
    h.magic = kEnvelopeMagic;
    h.version = kEnvelopeVersion;
    h.type = static_cast<std::uint8_t>(s.type);
    h.body_len = static_cast<std::uint32_t>(body.size());
    h.msg_id = s.msg_id;
    h.src = s.src;
    h.dst = s.dst;
    h.service_type = s.service;
    h.route_type = static_cast<std::uint8_t>(s.route);
    h.hops = s.hops;
    h.route_key = s.key;
    h.remaining_ms = s.remaining_ms;
    h.err_code = s.err;
    h.seq = s.seq;
    h.trace_id = 0xABCD0000 + s.seq;
    std::vector<std::byte> out(kEnvelopeHeadSize + body.size());
    WriteEnvelopeHead(out, h);
    if (!body.empty())
        std::memcpy(out.data() + kEnvelopeHeadSize, body.data(), body.size());
    return out;
}

std::optional<Msg> ParseMsg(const std::vector<std::byte>& bytes)
{
    Msg m;
    if (ParseEnvelope(bytes, m.head) != EnvStatus::kOk)
        return std::nullopt;
    m.body.assign(reinterpret_cast<const char*>(bytes.data()) + kEnvelopeHeadSize, bytes.size() - kEnvelopeHeadSize);
    return m;
}

// ---- 本机的 App（两种传输）----

class App
{
public:
    virtual ~App() = default;
    virtual bool Send(const std::vector<std::byte>& msg) = 0;
    virtual std::optional<std::vector<std::byte>> Recv(std::chrono::milliseconds timeout = 5s) = 0;
    std::optional<Msg> RecvMsg(std::chrono::milliseconds timeout = 5s)
    {
        auto b = Recv(timeout);
        if (!b)
            return std::nullopt;
        auto m = ParseMsg(*b);
        CHECK(m.has_value()); // Agent 转发出来的一定是合法信封
        return m;
    }
    bool Send(const Spec& s, std::string_view body = "payload") { return Send(Build(s, body)); }
    NodeId node = 0;
};

class ShmApp final : public App
{
public:
    using App::Send;
    ShmApp(const Endpoint& ep, NodeId n)
        : c_(MustConnect(ep, std::to_string(n)))
    {
        node = n;
    }
    bool Send(const std::vector<std::byte>& msg) override
    {
        auto r = c_.tx().TryReserve(static_cast<std::uint32_t>(msg.size()));
        if (r.status != shm::ReserveStatus::kOk)
            return false;
        std::memcpy(r.payload.data(), msg.data(), msg.size());
        c_.tx().Commit();
        return true;
    }
    std::optional<std::vector<std::byte>> Recv(std::chrono::milliseconds timeout) override
    {
        shm::RecordView v;
        if (!RecvOne(c_, v, timeout))
            return std::nullopt;
        std::vector<std::byte> out(v.payload, v.payload + v.len);
        c_.rx().Pop();
        return out;
    }
    ShmChannelClient& client() { return c_; }

private:
    ShmChannelClient c_;
};

class UdsApp final : public App
{
public:
    using App::Send;
    UdsApp(const Endpoint& ep, NodeId n)
        : conn_(Handshake(ep, std::to_string(n), Transport::kUdsOnly))
    {
        node = n;
        CHECK(conn_.result() == 0 && conn_.uds());
    }
    bool Send(const std::vector<std::byte>& msg) override
    {
        SendFrame(conn_.fd.get(), std::string_view(reinterpret_cast<const char*>(msg.data()), msg.size()));
        return true;
    }
    std::optional<std::vector<std::byte>> Recv(std::chrono::milliseconds timeout) override
    {
        auto f = RecvFrame(conn_.fd.get(), timeout);
        if (!f)
            return std::nullopt;
        return std::vector<std::byte>(reinterpret_cast<const std::byte*>(f->data()),
                                      reinterpret_cast<const std::byte*>(f->data()) + f->size());
    }
    void Close() { conn_.fd.reset(); }

private:
    RawConn conn_;
};

std::unique_ptr<App> MakeApp(bool shm, const Endpoint& ep, NodeId n)
{
    if (shm)
        return std::make_unique<ShmApp>(ep, n);
    return std::make_unique<UdsApp>(ep, n);
}

// ---- 用例 ----

void TestDirectForwardingMatrix()
{
    LeakCheck leak;
    AgentEnv env;
    NodeId next_node = 110; // 每个组合用新的 NodeId：保留期里旧通道还占着名字，仅 UDS 来要同名的会 EBUSY
    for (const bool from_shm : {true, false})
    {
        for (const bool to_shm : {true, false})
        {
            const NodeId from = next_node++;
            const NodeId to = next_node++;
            auto x = MakeApp(from_shm, env.ep, from);
            auto y = MakeApp(to_shm, env.ep, to);
            Spec s;
            s.src = from;
            s.dst = to;
            s.seq = 777;
            s.msg_id = 31;
            CHECK(x->Send(s, "hello-matrix"));
            auto m = y->RecvMsg();
            CHECK(m.has_value());
            CHECK(m->body == "hello-matrix");
            CHECK(m->head.src == from && m->head.dst == to && m->head.seq == 777 && m->head.msg_id == 31); // 都不改
            CHECK(m->head.type == static_cast<std::uint8_t>(EnvType::kOneway) && m->head.trace_id == 0xABCD0000 + 777);
            CHECK(m->head.hops == 1); // 转发一跳加一
            CHECK(m->head.remaining_ms > 4000 && m->head.remaining_ms <= 5000);
            // 反方向也通：回一条 Response。
            Spec r;
            r.type = EnvType::kResponse;
            r.src = to;
            r.dst = from;
            r.seq = 777;
            r.err = 0;
            CHECK(y->Send(r, "ok"));
            auto back = x->RecvMsg();
            CHECK(back.has_value() && back->body == "ok" && back->head.seq == 777 &&
                  back->head.type == static_cast<std::uint8_t>(EnvType::kResponse));
        }
    }
    // delivered_* 在写进目的地之后才加：收到消息时计数可能还没到（路由线程被抢占），要等；received_* 在处理之前就加了。
    CHECK(env.WaitStats([](const AgentRouterStats& st) { return st.delivered_shm + st.delivered_uds == 8; }));
    const AgentRouterStats st = env.router.stats();
    CHECK(st.received_shm + st.received_uds == 8);
    CHECK(st.bad_envelope == 0 && st.bad_src == 0 && st.stale_drops == 0 && st.no_route == 0);
}

void TestExpiry()
{
    LeakCheck leak;
    AgentEnv env;
    ShmApp a(env.ep, kA);
    ShmApp b(env.ep, kB);
    UdsApp c(env.ep, kC);
    Spec s;
    s.src = kA;
    s.dst = kB;

    // 排队 3 秒：剩余扣掉排队时间（约 2000）。
    env.SetClockOffset(3000ms);
    s.seq = 1;
    CHECK(a.Send(s, "aged"));
    auto m = b.RecvMsg();
    CHECK(m.has_value() && m->head.remaining_ms > 1500 && m->head.remaining_ms <= 2000);

    // 排队 6 秒：过期，不转发。
    env.SetClockOffset(6000ms);
    s.seq = 2;
    CHECK(a.Send(s, "expired"));
    CHECK(env.WaitStats([](const AgentRouterStats& st) { return st.stale_drops == 1; }));
    CHECK(!b.RecvMsg(300ms).has_value());

    // 过期的 Request：不回包（请求方早已判超时）；过期的 Response 也丢。
    s.type = EnvType::kRequest;
    s.seq = 3;
    CHECK(a.Send(s, "expired-req"));
    CHECK(env.WaitStats([](const AgentRouterStats& st) { return st.stale_drops == 2; }));
    CHECK(!a.RecvMsg(300ms).has_value());
    Spec r;
    r.type = EnvType::kResponse;
    r.src = kB;
    r.dst = kA;
    r.seq = 3;
    CHECK(b.Send(r, "late-response"));
    CHECK(env.WaitStats([](const AgentRouterStats& st) { return st.stale_drops == 3; }));
    CHECK(!a.RecvMsg(300ms).has_value());

    // remaining_ms == 0：无效，单独计数，不混进 stale_drops。
    env.SetClockOffset(0ms);
    s.type = EnvType::kOneway;
    s.seq = 4;
    s.remaining_ms = 0;
    CHECK(a.Send(s, "invalid"));
    CHECK(env.WaitStats([](const AgentRouterStats& st) { return st.invalid_expire == 1; }));
    CHECK(env.router.stats().stale_drops == 3);

    // 差一点用完（剩 1 毫秒、排队约 0）：还活着，转发出去的 remaining_ms 至少是 1，不会是 0。
    s.seq = 5;
    s.remaining_ms = 1;
    CHECK(a.Send(s, "last-ms"));
    auto edge = b.RecvMsg(2s);
    // 这条要么赶上（剩余 1）、要么因为排队超过 1ms 被判过期：两种都对，但绝不能转发出 remaining_ms == 0 的。
    if (edge.has_value())
        CHECK(edge->head.remaining_ms == 1);

    // 不是从环里来的消息（仅 UDS 的帧）没有入队时刻：排队时间记 0，拨快时钟也不会让它过期。
    env.SetClockOffset(6000ms);
    s.seq = 6;
    s.remaining_ms = 5000;
    s.dst = kA;
    s.src = kC;
    CHECK(c.Send(s, "from-uds"));
    auto u = a.RecvMsg();
    CHECK(u.has_value() && u.value().head.remaining_ms > 4000 && u.value().head.remaining_ms <= 5000);
    env.SetClockOffset(0ms);
}

void TestBadEnvelopeAndSpoofing()
{
    LeakCheck leak;
    AgentEnv env;
    ShmApp a(env.ep, kA);
    UdsApp c(env.ep, kC);
    ShmApp b(env.ep, kB);

    const auto garbage = [](std::size_t n)
    {
        std::vector<std::byte> v(n);
        for (std::size_t i = 0; i < n; ++i)
            v[i] = static_cast<std::byte>((i * 131 + 7) & 0xFF);
        return v;
    };
    Spec s;
    s.src = kA;
    s.dst = kB;
    std::uint64_t bad = 0;
    const auto expect_bad = [&](App& from, const std::vector<std::byte>& msg)
    {
        CHECK(from.Send(msg));
        ++bad;
        CHECK(env.WaitStats([&](const AgentRouterStats& st) { return st.bad_envelope == bad; }));
    };
    for (App* from : {static_cast<App*>(&a), static_cast<App*>(&c)})
    {
        expect_bad(*from, garbage(200));
        expect_bad(*from, garbage(10));
        expect_bad(*from, garbage(64));
        auto wrong_magic = Build(s);
        wrong_magic[0] ^= std::byte{1};
        expect_bad(*from, wrong_magic);
        auto wrong_len = Build(s, "abc");
        wrong_len.push_back(std::byte{0}); // body_len 比实际少 1
        expect_bad(*from, wrong_len);
        auto wrong_type = Build(s);
        wrong_type[5] = std::byte{9};
        expect_bad(*from, wrong_type);
    }
    CHECK(!b.RecvMsg(200ms).has_value()); // 一条都没转发出去
    // 坏消息没有毒化通道：好消息照常转发。
    CHECK(a.Send(s, "still-works"));
    CHECK(b.RecvMsg().value().body == "still-works");

    // 冒充：本机 App 的 src 必须是它自己。
    std::uint64_t spoof = 0;
    const auto expect_spoof = [&](App& from, NodeId claimed)
    {
        Spec f = s;
        f.src = claimed;
        CHECK(from.Send(f, "spoofed"));
        ++spoof;
        CHECK(env.WaitStats([&](const AgentRouterStats& st) { return st.bad_src == spoof; }));
    };
    expect_spoof(a, kC); // A 冒充 C
    expect_spoof(a, 0);  // 没有 src
    expect_spoof(c, kA); // 仅 UDS 的 C 冒充 A
    CHECK(!b.RecvMsg(200ms).has_value());
}

void TestNoRouteAndErrorReplies()
{
    LeakCheck leak;
    AgentEnv env;
    ShmApp a(env.ep, kA);
    UdsApp c(env.ep, kC);
    Spec s;
    s.type = EnvType::kRequest;
    s.src = kA;
    s.dst = 999; // 不存在
    s.seq = 55;
    s.msg_id = 123;
    CHECK(a.Send(s, "request"));
    auto e = a.RecvMsg();
    CHECK(e.has_value());
    CHECK(e->head.type == static_cast<std::uint8_t>(EnvType::kResponse) && e->head.err_code == kRouteErrNoRoute);
    CHECK(e->head.dst == kA && e->head.seq == 55 && e->head.msg_id == 123 && e->body.empty());
    CHECK(e->head.remaining_ms == 5000 && e->head.route_type == static_cast<std::uint8_t>(RouteType::kDirect));
    CHECK(e->head.trace_id == 0xABCD0000 + 55); // 追踪号带回去

    // 仅 UDS 的发送方同样收到错误回包。
    s.src = kC;
    s.seq = 56;
    CHECK(c.Send(s, "request"));
    auto e2 = c.RecvMsg();
    CHECK(e2.has_value() && e2->head.err_code == kRouteErrNoRoute && e2->head.seq == 56);

    // Oneway / Response 无路可走：丢，不通知。
    Spec o;
    o.src = kA;
    o.dst = 999;
    CHECK(a.Send(o, "oneway"));
    Spec r;
    r.type = EnvType::kResponse;
    r.src = kA;
    r.dst = 999;
    CHECK(a.Send(r, "response"));
    CHECK(env.WaitStats([](const AgentRouterStats& st) { return st.no_route == 4; }));
    CHECK(!a.RecvMsg(300ms).has_value());
    CHECK(env.router.stats().error_replies == 2);

    // Any / Hash / Broadcast 没有实例：同样无路可走（Request 回错误）。服务不存在、服务存在但实例列表是空的，两种都要。
    env.table.SetInstances(8, {});
    for (const std::uint16_t service : {std::uint16_t{9}, std::uint16_t{8}})
    for (const RouteType rt : {RouteType::kAny, RouteType::kHash, RouteType::kBroadcast})
    {
        Spec q;
        q.type = EnvType::kRequest;
        q.route = rt;
        q.src = kA;
        q.service = service;
        q.seq = 70 + static_cast<std::uint64_t>(rt) + 10u * service;
        CHECK(a.Send(q, "nobody"));
        auto er = a.RecvMsg();
        CHECK(er.has_value() && er->head.err_code == kRouteErrNoRoute && er->head.seq == q.seq);
    }
}

void TestTargetUnavailable()
{
    LeakCheck leak;
    AgentEnv env;
    ShmApp a(env.ep, kA);
    ShmApp b(env.ep, kB);
    UdsApp c(env.ep, kC);
    Spec s;
    s.src = kA;
    s.dst = kB;
    const std::string chunk(4000, 'f');

    // B 不读：Rx 环（1 MiB）被填满，之后的丢并计 rx_full。
    std::uint64_t sent = 0;
    while (env.router.stats().rx_full == 0 && sent < 2000)
    {
        s.seq = ++sent;
        if (!a.Send(s, chunk))
            std::this_thread::sleep_for(1ms); // A 的 Tx 满了（Agent 来不及搬）：等一下
    }
    CHECK(env.WaitStats([](const AgentRouterStats& st) { return st.rx_full > 0; }));
    // Request 送不进去：回 Unavailable，让调用方快速失败。
    Spec q = s;
    q.type = EnvType::kRequest;
    q.seq = 900001;
    CHECK(a.Send(q, chunk));
    auto e = a.RecvMsg();
    CHECK(e.has_value() && e->head.err_code == kRouteErrUnavailable && e->head.seq == 900001);
    // B 读走一批：又能送进去，没有卡死。
    std::uint64_t got = 0;
    while (b.RecvMsg(300ms).has_value())
        ++got;
    CHECK(got > 100);
    Spec after = s;
    after.seq = 900002;
    CHECK(a.Send(after, "after-drain"));
    auto m = b.RecvMsg();
    CHECK(m.has_value() && m->head.seq == 900002);

    // 目的地的单条上限（Rx 环的 max_payload 是 64 KiB）：太大的丢并计 too_large，Request 回 Unavailable。
    Spec big;
    big.type = EnvType::kRequest;
    big.src = kC;
    big.dst = kB;
    big.seq = 5;
    CHECK(c.Send(big, std::string(100 * 1024, 'z')));
    auto er = c.RecvMsg();
    CHECK(er.has_value() && er->head.err_code == kRouteErrUnavailable && er->head.seq == 5);
    CHECK(env.router.stats().too_large == 1);
}

void TestDetachedAndGoneTargets()
{
    LeakCheck leak;
    AgentEnv env;
    ShmApp a(env.ep, kA);
    auto b = std::make_unique<ShmApp>(env.ep, kB);
    UdsApp c(env.ep, kC);
    ShmClientOptions bopts = ClientOpts(env.ep, std::to_string(kB));
    Spec s;
    s.src = kA;
    s.dst = kB;

    // B 的 UDS 断了（进程没死）：通道进入保留，发给它的消息照常写进它的 Rx 环，等它回来读。
    CHECK(::shutdown(b->client().liveness_fd(), SHUT_RDWR) == 0);
    CHECK(WaitUntil([&] { return env.server->stats().detached_channels == 1; }));
    for (std::uint64_t i = 1; i <= 3; ++i)
    {
        s.seq = i;
        CHECK(a.Send(s, "while-detached"));
    }
    CHECK(env.WaitStats([](const AgentRouterStats& st) { return st.delivered_shm >= 3; }));
    CHECK(b->client().Reattach(bopts) == 0);
    for (std::uint64_t i = 1; i <= 3; ++i)
    {
        auto m = b->RecvMsg();
        CHECK(m.has_value() && m->head.seq == i && m->body == "while-detached");
    }

    // 保留期里 Rx 环写满：计 rx_full_detached（不是 rx_full）。
    CHECK(::shutdown(b->client().liveness_fd(), SHUT_RDWR) == 0);
    CHECK(WaitUntil([&] { return env.server->stats().detached_channels == 1; }));
    const std::string chunk(4000, 'd');
    for (std::uint64_t i = 100; env.router.stats().rx_full_detached == 0 && i < 2000; ++i)
    {
        s.seq = i;
        if (!a.Send(s, chunk))
            std::this_thread::sleep_for(1ms);
    }
    CHECK(env.WaitStats([](const AgentRouterStats& st) { return st.rx_full_detached > 0; }));
    CHECK(env.router.stats().rx_full == 0);

    // 仅 UDS 的 D 走了：它的 Node 撤销登记，之后发给它的是"无路可走"（Request 回错误）。
    {
        UdsApp gone(env.ep, kD);
        Spec t;
        t.src = kA;
        t.dst = kD;
        t.seq = 1;
        CHECK(a.Send(t, "to-d"));
        CHECK(gone.RecvMsg().value().body == "to-d");
    }
    CHECK(WaitUntil([&] { return env.server->stats().uds_channels == 1; })); // D 已断（C 还在）
    Spec t;
    t.type = EnvType::kRequest;
    t.src = kA;
    t.dst = kD;
    t.seq = 2;
    CHECK(a.Send(t, "to-gone"));
    auto er = a.RecvMsg();
    CHECK(er.has_value() && er->head.err_code == kRouteErrNoRoute && er->head.seq == 2);
    CHECK(env.WaitStats([](const AgentRouterStats& st) { return st.local_nodes == 3; })); // A B C
}

// 转发期间 inflight 标记是立着的：Agent 此刻崩溃，新 Agent 靠它跳过这一条（至多一次，也不会被毒包拖进崩溃循环）。
// 在路由表的 Resolve 里卡住路由器，从 App 这边（同一块共享内存）看控制块。
class BlockingTable final : public RouteTable
{
public:
    StaticRouteTable inner;
    std::mutex mu;
    std::condition_variable cv;
    bool armed = false, entered = false, open = true;

    void Resolve(const EnvelopeHead& h, std::vector<NodeId>& out) override
    {
        {
            std::unique_lock lk(mu);
            if (armed)
            {
                armed = false;
                entered = true;
                cv.notify_all();
                cv.wait(lk, [&] { return open; });
            }
        }
        inner.Resolve(h, out);
    }
    ConnectionHandle RemoteOf(NodeId n) const override { return inner.RemoteOf(n); }
};

void TestInflightMarkedWhileForwarding()
{
    LeakCheck leak;
    TempDir dir;
    BlockingTable table;
    AgentRouter router(table);
    LengthPrefixCodecFactory codec(1 << 20);
    EventLoopGroup group(EventLoopGroupOptions{1, 256});
    CHECK(group.Start() == 0);
    const Endpoint ep = dir.Unix("agent.sock");
    ShmUdsDataPath path;
    path.handler = &router;
    path.factory = &codec;
    ShmChannelServer server(group, ep, Ret(), router, path);
    CHECK(server.Start() == 0);
    {
        ShmApp a(ep, kA);
        ShmApp b(ep, kB);
        shm::RingControl& tx = a.client().shm().tx_control();
        { // 没有在处理的记录：标记是哨兵或落后于 read_idx。
            const std::uint64_t in = tx.inflight_idx.load();
            CHECK(in == shm::kNoInflight || in < tx.read_idx.load() || in == tx.read_idx.load());
        }
        {
            const std::lock_guard lk(table.mu);
            table.armed = true;
            table.entered = false;
            table.open = false;
        }
        Spec s;
        s.src = kA;
        s.dst = kB;
        CHECK(a.Send(s, "marked"));
        {
            std::unique_lock lk(table.mu);
            CHECK(table.cv.wait_for(lk, 5s, [&] { return table.entered; }));
        }
        // 路由器正在处理这条：它的游标 == read_idx（还没 Pop），inflight 必须指着它。
        CHECK(tx.write_idx.load() > tx.read_idx.load());
        CHECK(tx.inflight_idx.load() == tx.read_idx.load());
        const std::uint64_t cursor = tx.read_idx.load();
        {
            const std::lock_guard lk(table.mu);
            table.open = true;
            table.cv.notify_all();
        }
        CHECK(b.RecvMsg().value().body == "marked");
        CHECK(WaitUntil([&] { return tx.read_idx.load() > cursor; })); // 处理完才 Pop
    }
}

// 同一个 Node 既在本机又被登记成远端（登记过期了）：本机的优先，不能绕一圈经链路发出去。
void TestLocalWinsOverRemote()
{
    LeakCheck leak;
    AgentEnv env;
    ShmApp a(env.ep, kA);
    ShmApp b(env.ep, kB);
    env.table.SetRemote(kB, ConnectionHandle(0x1234567)); // 一个不会解析到任何连接的句柄
    Spec s;
    s.src = kA;
    s.dst = kB;
    CHECK(a.Send(s, "local-first"));
    CHECK(b.RecvMsg().value().body == "local-first");
    CHECK(env.router.stats().delivered_link == 0 && env.router.stats().send_closed == 0);
}

// discard_retained 重建：新通道先于旧通道的 OnClosed 出现，旧的撤销时不能把新的登记也删了。
void TestDiscardRebuildKeepsRegistration()
{
    LeakCheck leak;
    AgentEnv env;
    ShmApp b(env.ep, kB);
    {
        ShmApp a(env.ep, kA);
        CHECK(::shutdown(a.client().liveness_fd(), SHUT_RDWR) == 0);
        CHECK(WaitUntil([&] { return env.server->stats().detached_channels == 1; }));
    } // 旧的 A 彻底走了，保留中的通道还在
    ShmClientOptions o = ClientOpts(env.ep, std::to_string(kA));
    o.discard_retained = true;
    auto fresh = ShmChannelClient::Connect(o);
    CHECK(fresh.has_value() && !fresh->resumed());
    CHECK(WaitUntil([&] { return env.server->stats().discarded == 1 && env.server->stats().open_channels == 2; }));
    Spec s;
    s.src = kB;
    s.dst = kA;
    CHECK(b.Send(s, "to-new-a"));
    shm::RecordView v;
    CHECK(RecvOne(*fresh, v));
    CHECK(ParseMsg(std::vector<std::byte>(v.payload, v.payload + v.len)).value().body == "to-new-a");
    fresh->rx().Pop();
    CHECK(env.router.stats().local_nodes == 2);
}

void TestHops()
{
    LeakCheck leak;
    AgentEnv env;
    ShmApp a(env.ep, kA);
    ShmApp b(env.ep, kB);
    Spec s;
    s.src = kA;
    s.dst = kB;
    s.hops = kMaxHops - 1;
    s.seq = 1;
    CHECK(a.Send(s, "last-hop"));
    auto m = b.RecvMsg();
    CHECK(m.has_value() && m->head.hops == kMaxHops); // 7 → 8：最后一跳
    s.hops = kMaxHops;
    s.seq = 2;
    CHECK(a.Send(s, "too-many"));
    CHECK(env.WaitStats([](const AgentRouterStats& st) { return st.hops_exceeded == 1; }));
    CHECK(!b.RecvMsg(200ms).has_value());
}

void TestRoutingModes()
{
    LeakCheck leak;
    AgentEnv env;
    ShmApp a(env.ep, kA);
    ShmApp b(env.ep, kB);
    UdsApp c(env.ep, kC);
    ShmApp d(env.ep, kD);
    env.table.SetInstances(5, {kD, kB, kC, kB}); // 乱序、重复：内部排序去重 → B C D
    std::array<App*, 3> inst{&b, &c, &d};

    // Any：轮询。
    Spec s;
    s.src = kA;
    s.route = RouteType::kAny;
    s.service = 5;
    std::array<int, 3> got{};
    for (std::uint64_t i = 0; i < 9; ++i)
    {
        s.seq = i;
        CHECK(a.Send(s, "any"));
    }
    for (std::size_t k = 0; k < 3; ++k)
    {
        for (int i = 0; i < 3; ++i)
        {
            auto m = inst[k]->RecvMsg();
            CHECK(m.has_value());
            ++got[k];
            CHECK(m->head.seq % 3 == k); // 严格轮询：第 i 条给第 i % 3 个实例
        }
    }
    CHECK(got == (std::array<int, 3>{3, 3, 3}));

    // Hash：同一个键永远落在同一个实例，键 k → 排序后第 k % 3 个。
    s.route = RouteType::kHash;
    for (std::uint64_t key = 0; key < 9; ++key)
    {
        s.seq = key;
        s.key = key;
        CHECK(a.Send(s, "hash"));
        auto m = inst[key % 3]->RecvMsg();
        CHECK(m.has_value() && m->head.seq == key);
    }

    // Broadcast：全部实例各一份；发送方自己是实例时不发给自己。
    s.route = RouteType::kBroadcast;
    s.seq = 500;
    CHECK(a.Send(s, "bcast"));
    for (App* x : inst)
    {
        auto m = x->RecvMsg();
        CHECK(m.has_value() && m->head.seq == 500 && m->body == "bcast");
    }
    Spec from_b = s;
    from_b.src = kB;
    from_b.seq = 501;
    CHECK(b.Send(from_b, "bcast2"));
    CHECK(c.RecvMsg().value().head.seq == 501);
    CHECK(d.RecvMsg().value().head.seq == 501);
    CHECK(!b.RecvMsg(200ms).has_value());
}

void TestNodeMapping()
{
    LeakCheck leak;
    AgentRouterOptions ro;
    ro.node_of = [](std::string_view busid, const PeerCredentials&) -> std::optional<NodeId>
    {
        if (busid == "svc-a" || busid == "svc-a-alias")
            return kA; // 两个通道名映射到同一个 Node
        if (busid == "svc-b")
            return kB;
        return std::nullopt;
    };
    AgentEnv env(ro);
    ShmClientOptions oa = ClientOpts(env.ep, "svc-a");
    auto a = ShmChannelClient::Connect(oa);
    CHECK(a.has_value());
    CHECK(Handshake(env.ep, "unknown-name", Transport::kShmRequired).result() == EACCES);   // 不认识
    CHECK(Handshake(env.ep, "unknown-uds", Transport::kUdsOnly).result() == EACCES);
    CHECK(Handshake(env.ep, "svc-a-alias", Transport::kShmRequired).result() == EBUSY);     // 另一个名字映射到了同一个 Node
    CHECK(Handshake(env.ep, "svc-a-alias", Transport::kUdsOnly).result() == EBUSY);
    // 登记按映射后的 NodeId：用 "svc-b" 连上、src 填 kB 才合法。
    ShmClientOptions ob = ClientOpts(env.ep, "svc-b");
    auto b = ShmChannelClient::Connect(ob);
    CHECK(b.has_value());
    CHECK(env.WaitStats([](const AgentRouterStats& st) { return st.local_nodes == 2; }));
    Spec s;
    s.src = kA;
    s.dst = kB;
    auto bytes = Build(s, "mapped");
    auto r = a->tx().TryReserve(static_cast<std::uint32_t>(bytes.size()));
    CHECK(r.status == shm::ReserveStatus::kOk);
    std::memcpy(r.payload.data(), bytes.data(), bytes.size());
    a->tx().Commit();
    shm::RecordView v;
    CHECK(RecvOne(*b, v));
    CHECK(ParseMsg(std::vector<std::byte>(v.payload, v.payload + v.len)).value().body == "mapped");
    b->rx().Pop();
}

// 顺序与数量：大量消息按序到达、一条不少（多个通道、两种传输同时发）。
void TestOrderAndVolume()
{
    LeakCheck leak;
    AgentEnv env;
    ShmApp a(env.ep, kA);
    UdsApp c(env.ep, kC);
    ShmApp b(env.ep, kB);
    constexpr std::uint64_t kN = 20000;
    constexpr std::uint64_t kWindow = 2000; // 在途上限：目的地的 Rx 环满了路由器会（合法地）丢，这里要的是"不丢"，所以发送方按已消费数流控
    std::atomic<std::uint64_t> consumed_a{0}, consumed_c{0};
    std::thread sender_a(
        [&]
        {
            Spec s;
            s.src = kA;
            s.dst = kB;
            for (std::uint64_t i = 0; i < kN; ++i)
            {
                while (i >= consumed_a.load(std::memory_order_acquire) + kWindow)
                    std::this_thread::yield();
                s.seq = i;
                while (!a.Send(s, "a-to-b"))
                    std::this_thread::yield();
            }
        });
    std::thread sender_c(
        [&]
        {
            Spec s;
            s.src = kC;
            s.dst = kB;
            for (std::uint64_t i = 0; i < kN; ++i)
            {
                while (i >= consumed_c.load(std::memory_order_acquire) + kWindow)
                    std::this_thread::yield();
                s.seq = kN + i;
                CHECK(c.Send(s, "c-to-b"));
            }
        });
    std::uint64_t next_a = 0, next_c = kN, total = 0;
    while (total < 2 * kN)
    {
        auto m = b.RecvMsg(10s);
        CHECK(m.has_value());
        if (m->head.src == kA)
        {
            CHECK(m->head.seq == next_a++); // 同一个发送方的消息保序
            consumed_a.store(next_a, std::memory_order_release);
        }
        else
        {
            CHECK(m->head.seq == next_c++);
            consumed_c.store(next_c - kN, std::memory_order_release);
        }
        ++total;
    }
    sender_a.join();
    sender_c.join();
    CHECK(next_a == kN && next_c == 2 * kN);
    CHECK(env.WaitStats([&](const AgentRouterStats& st) { return st.delivered_shm + st.delivered_uds == 2 * kN; })); // 计数晚于收发
    const AgentRouterStats st = env.router.stats();
    CHECK(st.rx_full == 0 && st.bad_envelope == 0);
}

// ---- 两个 Agent 之间的链路（TCP）----

// 把链路连接的句柄记下来（登记进路由表要用），其余原样交给路由器。
class LinkHandler final : public IConnectionHandler
{
public:
    explicit LinkHandler(AgentRouter& r)
        : router_(r)
    {
    }
    void OnConnected(IConnection& c) noexcept override
    {
        {
            const std::lock_guard lk(mu_);
            handle_ = c.handle();
        }
        router_.OnConnected(c);
    }
    void OnMessage(IConnection& c, std::span<const std::byte> p) noexcept override { router_.OnMessage(c, p); }
    void OnClosed(IConnection& c, CloseReason r, int e) noexcept override { router_.OnClosed(c, r, e); }
    ConnectionHandle WaitHandle()
    {
        CHECK(WaitUntil(
            [&]
            {
                const std::lock_guard lk(mu_);
                return handle_.raw() != 0;
            }));
        const std::lock_guard lk(mu_);
        return handle_;
    }

private:
    AgentRouter& router_;
    std::mutex mu_;
    ConnectionHandle handle_;
};

void TestRemoteLink()
{
    LeakCheck leak;
    constexpr NodeId kRemoteApp = 201;
    AgentEnv a1;
    AgentEnv a2;
    // a2 监听 TCP；a1 去连它。两边的链路都交给各自的路由器。
    LinkHandler link2(a2.router);
    LinkHandler link1(a1.router);
    LengthPrefixCodecFactory link_codec(1 << 20);
    const auto listen_ep = TcpEndpoint::From(*Endpoint::Tcp("127.0.0.1", 0));
    CHECK(listen_ep.has_value());
    TcpServer server(a2.group, *listen_ep, TcpServerOptions{}, link2, link_codec);
    CHECK(server.Start() == 0);
    const auto actual = TcpEndpoint::From(server.endpoint());
    CHECK(actual.has_value());
    TcpClient client(a1.group, *actual, TcpClientOptions{}, ReconnectPolicy{}, link1, link_codec);
    CHECK(client.Start() == 0);
    // 路由表：a1 认为 201 / 202 在远端（经这条链路）；a2 认为 App 101 在远端（经它这一端的链路）。
    // 路由表只能在路由器所在的 loop 线程上改（这里整条链路还没有流量，直接改是安全的）。
    a1.table.SetRemote(kRemoteApp, link1.WaitHandle());
    a1.table.SetRemote(202, link1.WaitHandle());
    a2.table.SetRemote(kA, link2.WaitHandle());

    ShmApp app1(a1.ep, kA);
    UdsApp app2(a2.ep, kRemoteApp);

    // 跨 Agent 的单向：App1 (shm, a1) → App201 (uds, a2)。
    Spec s;
    s.src = kA;
    s.dst = kRemoteApp;
    s.seq = 9;
    CHECK(app1.Send(s, "across"));
    auto m = app2.RecvMsg();
    CHECK(m.has_value() && m->body == "across" && m->head.src == kA && m->head.seq == 9);
    CHECK(m->head.hops == 2);                                             // a1 一跳、a2 一跳
    CHECK(m->head.remaining_ms > 4000 && m->head.remaining_ms <= 5000);
    // 回包走反方向：App201 → a2 → 链路 → a1 → App1 (shm)。
    Spec r;
    r.type = EnvType::kResponse;
    r.src = kRemoteApp;
    r.dst = kA;
    r.seq = 9;
    CHECK(app2.Send(r, "back"));
    auto back = app1.RecvMsg();
    CHECK(back.has_value() && back->body == "back" && back->head.seq == 9 && back->head.hops == 2);

    // 远端无路可走（202 在 a1 的表里指向链路，但 a2 不认识）：a2 回的错误 Response 经链路回来，a1 再转给 App1。
    Spec q;
    q.type = EnvType::kRequest;
    q.src = kA;
    q.dst = 202;
    q.seq = 33;
    CHECK(app1.Send(q, "who?"));
    auto e = app1.RecvMsg();
    CHECK(e.has_value() && e->head.err_code == kRouteErrNoRoute && e->head.seq == 33 && e->head.dst == kA);
    CHECK(a1.WaitStats([](const AgentRouterStats& st) { return st.delivered_link >= 2 && st.received_link >= 2; })); // 计数晚于收发
    CHECK(a2.WaitStats([](const AgentRouterStats& st) { return st.received_link >= 2; }));

    // 过期在链路上同样生效：a1 拨快时钟，从环里来的消息排队 6 秒，到不了 a2。
    a1.SetClockOffset(6000ms);
    s.seq = 10;
    CHECK(app1.Send(s, "stale-across"));
    CHECK(a1.WaitStats([](const AgentRouterStats& st) { return st.stale_drops == 1; }));
    CHECK(!app2.RecvMsg(300ms).has_value());
    a1.SetClockOffset(0ms);

    // 连接归 group 持有：先把两个 group 停掉（连接全部关闭、OnClosed 回调完），处理器才能销毁。
    client.Stop();
    server.Stop();
    a1.group.Shutdown(2s);
    a2.group.Shutdown(2s);
}

} // namespace

#define RUN(t)                                                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        const char* only = std::getenv("ONLY");                                                                        \
        if (only != nullptr && std::strstr(#t, only) == nullptr)                                                       \
            break;                                                                                                     \
        std::fprintf(stderr, "[run] %s\n", #t);                                                                        \
        t();                                                                                                           \
    } while (0)

int main()
{
    RUN(TestDirectForwardingMatrix);
    RUN(TestExpiry);
    RUN(TestBadEnvelopeAndSpoofing);
    RUN(TestNoRouteAndErrorReplies);
    RUN(TestTargetUnavailable);
    RUN(TestDetachedAndGoneTargets);
    RUN(TestInflightMarkedWhileForwarding);
    RUN(TestLocalWinsOverRemote);
    RUN(TestDiscardRebuildKeepsRegistration);
    RUN(TestHops);
    RUN(TestRoutingModes);
    RUN(TestNodeMapping);
    RUN(TestOrderAndVolume);
    RUN(TestRemoteLink);
    std::puts("agent_router_test: OK");
    return 0;
}
