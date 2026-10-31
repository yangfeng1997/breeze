#include "base/net/rudp/rudp_server.h"

#include <netinet/in.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <random>
#include <thread>
#include <utility>
#include <vector>

#include "base/container/flat_hash_map.h"
#include "base/logger/logger.h"
#include "base/logger/logger_macros.h"
#include "base/net/loop/group_access.h"
#include "base/net/rudp/internal/rudp_connection.h"
#include "base/net/rudp/internal/ticket_table.h"
#include "base/net/rudp/internal/udp_socket.h"
#include "base/net/socket/socket_address.h"

namespace breeze
{

namespace
{

// 会话按来源 ip:port 固定。IPv6 的 scope id 不参与（链路本地地址在服务端场景里没有意义）。
struct AddrKey
{
    std::uint16_t family = 0;
    std::uint16_t port = 0; // 网络序，只用来比较
    std::array<std::uint8_t, 16> ip{};

    bool operator==(const AddrKey&) const = default;

    static std::optional<AddrKey> From(const sockaddr_storage& ss, socklen_t len)
    {
        AddrKey k;
        if (ss.ss_family == AF_INET && len >= sizeof(sockaddr_in))
        {
            sockaddr_in in;
            std::memcpy(&in, &ss, sizeof(in));
            k.family = AF_INET;
            k.port = in.sin_port;
            std::memcpy(k.ip.data(), &in.sin_addr, 4);
            return k;
        }
        if (ss.ss_family == AF_INET6 && len >= sizeof(sockaddr_in6))
        {
            sockaddr_in6 in6;
            std::memcpy(&in6, &ss, sizeof(in6));
            k.family = AF_INET6;
            k.port = in6.sin6_port;
            std::memcpy(k.ip.data(), &in6.sin6_addr, 16);
            return k;
        }
        return std::nullopt;
    }
};

// 每个数据报都要查一次表，所以哈希要便宜：键只有 20 字节，两次 64 位乘法混合就够，不用通用的字节哈希
// （std::hash<string_view> 在压测里占服务端用户态指令的 5%）。带随机种子：FlatHashMap 没有洪泛防护，
// 键来自网络；表里只有通过票据的来源，种子是便宜的保险。
struct AddrKeyHash
{
    std::uint64_t seed = 0;

    std::size_t operator()(const AddrKey& k) const noexcept
    {
        std::uint64_t a, b;
        std::memcpy(&a, k.ip.data(), 8);
        std::memcpy(&b, k.ip.data() + 8, 8);
        std::uint64_t x = (a ^ seed) * 0x9E3779B97F4A7C15ull;
        x ^= (b + ((static_cast<std::uint64_t>(k.port) << 16) | k.family)) * 0xC2B2AE3D27D4EB4Full;
        x ^= x >> 32;
        x *= 0xD6E8FEB86659FD93ull;
        x ^= x >> 29;
        return static_cast<std::size_t>(x);
    }
};

std::uint64_t RandomSeed()
{
    std::random_device rd;
    return (static_cast<std::uint64_t>(rd()) << 32) | rd();
}
static_assert(sizeof(AddrKey) == 20, "AddrKeyHash 假定 ip 数组之前是 4 字节（family 与 port）");

} // namespace

struct RUdpServer::Impl final : IDatagramHandler, RUdpConnection::Owner
{
    Impl(EventLoopGroup& g, Endpoint ep, RUdpServerOptions o, IConnectionHandler& h)
        : group(g)
        , endpoint(std::move(ep))
        , options(std::move(o))
        , handler(h)
        , tickets(options.max_pending_tickets)
        , by_addr(0, AddrKeyHash{RandomSeed()})
    {
    }

    struct Session
    {
        RUdpConnection* conn;
        AddrKey key;
    };

    EventLoopGroup& group;
    Endpoint endpoint;
    RUdpServerOptions options;
    IConnectionHandler& handler;
    TicketTable tickets;

    // 以下只在 loop 线程访问（Start 时选定 loop 之前除外）。
    EventLoop* loop = nullptr;
    std::unique_ptr<UdpSocket> socket;
    FlatHashMap<AddrKey, std::size_t, AddrKeyHash> by_addr; // → sessions 下标
    std::vector<Session> sessions;
    std::vector<RUdpConnection*> active; // 要 Tick 的会话（有事可做的），静止的不在里面；tick_index 是它在这里的下标
    EventLoop::TimerId tick_timer = 0;
    EventLoop::HookId hook = 0;
    bool accepting = false;
    // 会话数：loop 线程写，析构线程等它归零。
    std::atomic<std::uint32_t> live{0};

    // 过载时每秒可能有成千上万个 Hello 被拒，日志限频。
    EventLoop::Clock::time_point last_reject_log{};
    std::uint64_t suppressed_rejects = 0;

    int StartOnLoop();
    void StopAccepting();
    void CloseAll();
    void MaybeCloseSocket();
    void ArmTick();
    void TickAll();
    void RemoveActive(std::size_t i);
    bool ShouldLogReject();
    void Reply(const sockaddr_storage& to, socklen_t to_len, const rudp::Control& c);
    void HandleHello(const sockaddr_storage& src, socklen_t len, const AddrKey& key, const rudp::Packet& p);
    void UnknownSource(const sockaddr_storage& src, socklen_t len, const rudp::Packet& p);

    void OnDatagram(const sockaddr_storage& src, socklen_t len, std::span<const std::byte> data) override;
    void OnConnectionDetached(RUdpConnection& conn) noexcept override;
    void OnConnectionWake(RUdpConnection& conn) noexcept override;
};

int RUdpServer::Impl::StartOnLoop()
{
    if (const int e = UdpSocket::OpenServer(*loop, endpoint, options.rudp, *this, socket); e != 0)
        return e;
    accepting = true;
    // 停机时 loop 自己停掉接受：group 不认识 RUdpServer，只认钩子。socket 要留着：优雅关闭的会话还要收 ack、发 Close。
    hook = loop->AddShutdownHook(
        [this]
        {
            hook = 0; // 钩子执行完就被清掉了
            StopAccepting();
        });
    return 0;
}

void RUdpServer::Impl::StopAccepting()
{
    accepting = false;
    if (hook != 0)
    {
        loop->RemoveShutdownHook(hook);
        hook = 0;
    }
    MaybeCloseSocket();
}

// 不再接受、且没有会话了：关 socket，端口随之释放。
void RUdpServer::Impl::MaybeCloseSocket()
{
    if (!accepting && sessions.empty() && socket)
        socket->Close();
}

void RUdpServer::Impl::CloseAll()
{
    StopAccepting();
    // Close 只是标记并排进回收列表，不会摘表，所以遍历期间 sessions 不变。
    for (const Session& s : sessions)
        s.conn->Close();
}

bool RUdpServer::Impl::ShouldLogReject()
{
    const auto now = EventLoop::Clock::now();
    if (now - last_reject_log < std::chrono::seconds(1))
    {
        ++suppressed_rejects;
        return false;
    }
    last_reject_log = now;
    return true;
}

void RUdpServer::Impl::Reply(const sockaddr_storage& to, socklen_t to_len, const rudp::Control& c)
{
    (void)socket->Send({}, c.view(), &to, to_len); // 失败（缓冲满）就丢：对端会重发
}

// ---- 收 ----

void RUdpServer::Impl::OnDatagram(const sockaddr_storage& src, socklen_t len, std::span<const std::byte> data)
{
    LoopStats& st = loop->stats();
    const auto packet = rudp::Parse(data);
    const auto key = AddrKey::From(src, len);
    if (!packet || !key)
    {
        LoopStats::Bump(st.datagrams_dropped);
        return;
    }
    if (packet->type == rudp::PacketType::Hello)
    {
        HandleHello(src, len, *key, *packet);
        return;
    }
    const auto it = by_addr.find(*key);
    if (it == by_addr.end())
    {
        UnknownSource(src, len, *packet);
        return;
    }
    sessions[it->second].conn->OnPacket(*packet);
}

// 没有会话的来源发来数据：丢掉。Data / Ping 回一个 Close，让重启前的客户端立刻知道会话没了，
// 而不是等心跳超时。Close 比触发它的包小，不会被用来放大流量。
void RUdpServer::Impl::UnknownSource(const sockaddr_storage& src, socklen_t len, const rudp::Packet& p)
{
    LoopStats::Bump(loop->stats().datagrams_dropped);
    std::uint32_t conv = 0;
    if (p.type == rudp::PacketType::Data)
        conv = rudp::PeekKcpConv(p.body);
    else if (p.type == rudp::PacketType::Ping)
        conv = p.conv;
    if (conv != 0)
        Reply(src, len, rudp::EncodeClose(conv, static_cast<std::uint8_t>(CloseReason::PeerClosed)));
}

void RUdpServer::Impl::HandleHello(const sockaddr_storage& src,
                                   socklen_t len,
                                   const AddrKey& key,
                                   const rudp::Packet& p)
{
    LoopStats& st = loop->stats();
    // 同一来源已经有会话：Hello 是客户端没收到 Accept 的重传，重发 Accept（幂等）。conv 不同的不理。
    if (const auto it = by_addr.find(key); it != by_addr.end())
    {
        RUdpConnection* c = sessions[it->second].conn;
        if (c->conv() == p.conv && !c->closed())
            Reply(src, len, rudp::EncodeAccept(c->conv()));
        else
            LoopStats::Bump(st.datagrams_dropped);
        return;
    }
    if (!accepting)
    {
        Reply(src, len, rudp::EncodeReject(rudp::RejectReason::Stopping));
        return;
    }
    if (p.version != rudp::kVersion)
    {
        Reply(src, len, rudp::EncodeReject(rudp::RejectReason::Version));
        return;
    }

    PeerInfo info;
    info.endpoint = sock::FromSockaddr(src, len, Endpoint::Kind::Udp);
    if (options.accept_filter && !options.accept_filter(info))
    {
        LoopStats::Bump(st.rejected_filter);
        Reply(src, len, rudp::EncodeReject(rudp::RejectReason::Filtered));
        return;
    }

    // 先占名额再验票据：连接表满时不消耗票据，客户端稍后还能用同一张重试。
    // 验票失败再归还（同一线程，没人看得到中间态）。
    if (!loop->table().TryReserve())
    {
        LoopStats::Bump(st.rejected_full);
        if (ShouldLogReject())
            LOG_WARN("连接表已满，拒绝新会话 endpoint={}（此前被限频略去 {} 条）",
                     socket->local().ToString(),
                     std::exchange(suppressed_rejects, 0));
        Reply(src, len, rudp::EncodeReject(rudp::RejectReason::Busy));
        return;
    }
    if (!tickets.Consume(p.conv, p.token))
    {
        loop->table().Unreserve();
        LoopStats::Bump(st.rejected_ticket);
        Reply(src, len, rudp::EncodeReject(rudp::RejectReason::Ticket));
        return;
    }

    RUdpConnection* c = RUdpConnection::Create(
        *loop, *socket, &src, len, p.conv, std::move(info), handler, *this, options.connection, options.rudp);
    if (c == nullptr)
    {
        loop->table().Unreserve(); // KCP 建不出来（内存不足）：票据已用掉，客户端要换一张
        LoopStats::Bump(st.rejected_resource);
        Reply(src, len, rudp::EncodeReject(rudp::RejectReason::Busy));
        return;
    }
    c->owner_index = sessions.size();
    sessions.push_back({c, key});
    by_addr.emplace(key, c->owner_index);
    live.fetch_add(1, std::memory_order_relaxed);
    // Accept 先于 OnConnected 发出：OnConnected 里就能 Send，数据排在 Accept 之后，客户端不会先收到数据。
    Reply(src, len, rudp::EncodeAccept(p.conv));
    c->Establish();
    LoopStats::Bump(st.accepted);
}

// ---- 摘表（会话回收前，由 NotifyClosed 触发）----

void RUdpServer::Impl::OnConnectionDetached(RUdpConnection& conn) noexcept
{
    if (conn.ticking())
        RemoveActive(conn.tick_index); // 关了但还没被 TickAll 清走：回收前必须摘掉，否则活跃表里是悬空指针
    const std::size_t i = conn.owner_index;
    by_addr.erase(sessions[i].key);
    if (i + 1 != sessions.size())
    {
        sessions[i] = sessions.back();
        sessions[i].conn->owner_index = i;
        by_addr[sessions[i].key] = i;
    }
    sessions.pop_back();
    live.fetch_sub(1, std::memory_order_release);
    MaybeCloseSocket();
}

// ---- 定时推进 ----

void RUdpServer::Impl::ArmTick()
{
    if (tick_timer != 0)
        return;
    tick_timer = loop->RunAfter(std::chrono::milliseconds(options.rudp.interval_ms),
                                [this]
                                {
                                    tick_timer = 0;
                                    TickAll();
                                });
}

void RUdpServer::Impl::OnConnectionWake(RUdpConnection& conn) noexcept
{
    conn.tick_index = active.size();
    active.push_back(&conn);
    ArmTick();
}

void RUdpServer::Impl::RemoveActive(std::size_t i)
{
    RUdpConnection* last = active.back();
    active.pop_back();
    if (i < active.size())
    {
        active[i] = last;
        last->tick_index = i;
    }
}

// 只走活跃的会话（有事可做的）；静止的不在表里，空闲会话不占 CPU。没有活跃会话就不再装定时器，下一次唤醒时再装。
// Tick 里的关闭只标记、不摘表（摘表在回收阶段）；回调里可能唤醒别的会话（push_back），所以按下标遍历、每轮重读大小，
// 新加的在本轮就会被走到。
void RUdpServer::Impl::TickAll()
{
    const std::uint32_t now = KcpNowMs();
    for (std::size_t i = 0; i < active.size();)
    {
        if (active[i]->Tick(now))
            ++i;
        else
            RemoveActive(i); // 换上来的那个还没 Tick 过，下标不动
    }
    if (!active.empty())
        ArmTick();
}

// ---- 对外 ----

RUdpServer::RUdpServer(EventLoopGroup& group,
                       const RUdpEndpoint& endpoint,
                       RUdpServerOptions options,
                       IConnectionHandler& handler)
    : endpoint_(endpoint.endpoint())
    , impl_(std::make_unique<Impl>(group, endpoint.endpoint(), std::move(options), handler))
{
}

RUdpServer::~RUdpServer()
{
    Impl& m = *impl_;
    if (m.loop == nullptr || !m.group.running())
        return; // 没 Start 过；或 group 已 Shutdown：会话早已回收，停机钩子也处理过了
    // 同步关：不然 Stop 投递的任务可能在本对象销毁之后才在 loop 上跑；会话里有指向 Impl 的指针，也要等它们回收完。
    (void)m.loop->CallSync(
        [&m]
        {
            m.CloseAll();
            return 0;
        });
    for (int i = 0; i < 5000 && m.live.load(std::memory_order_acquire) != 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    // 最后一次 TickAll 之后装的定时器还挂在 loop 上、捕获着 this：摘掉，否则它会在本对象销毁之后触发。
    (void)m.loop->CallSync(
        [&m]
        {
            if (m.tick_timer != 0)
                m.loop->Cancel(m.tick_timer);
            m.tick_timer = 0;
            return 0;
        });
}

int RUdpServer::Start()
{
    Impl& m = *impl_;
    if (!m.options.rudp.Valid())
        return EINVAL;
    if (!m.group.running())
    {
        if (m.group.stopping())
            return ESHUTDOWN;
        LOG_FATAL("RUdpServer::Start 之前必须已 EventLoopGroup::Start() 成功 endpoint={}", m.endpoint.ToString());
    }
    if (m.loop != nullptr)
        LOG_FATAL("RUdpServer 重复 Start endpoint={}", m.endpoint.ToString());

    m.loop = &GroupAccess::PickLoop(m.group);
    // bind 与登记 epoll 只能在 loop 线程上做；loop 已在运行，投过去等结果。
    if (const int rc = m.loop->CallSync([&m] { return m.StartOnLoop(); }); rc != 0)
    {
        m.loop = nullptr;
        return rc;
    }
    endpoint_ = m.socket->local();
    return 0;
}

void RUdpServer::Stop()
{
    Impl& m = *impl_;
    if (m.loop == nullptr)
        return;
    m.loop->Post([&m] { m.StopAccepting(); });
}

RUdpEndpoint RUdpServer::local_endpoint() const
{
    // endpoint_ 由构造时的 RUdpEndpoint 而来，绑定后只是填上实际端口，种类不会变，所以转换一定成功。
    return *RUdpEndpoint::From(endpoint_);
}

std::optional<RUdpTicket> RUdpServer::IssueTicket(std::chrono::milliseconds ttl) { return impl_->tickets.Issue(ttl); }

bool RUdpServer::AddTicket(const RUdpTicket& ticket, std::chrono::milliseconds ttl)
{
    return impl_->tickets.Add(ticket, ttl);
}

std::size_t RUdpServer::pending_tickets() const { return impl_->tickets.size(); }

} // namespace breeze
