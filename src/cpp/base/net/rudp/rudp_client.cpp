#include "base/net/rudp/rudp_client.h"

#include <cerrno>
#include <thread>
#include <utility>

#include "base/logger/logger.h"
#include "base/logger/logger_macros.h"
#include "base/net/loop/group_access.h"
#include "base/net/rudp/internal/rudp_connection.h"
#include "base/net/rudp/internal/udp_socket.h"

namespace breeze
{

namespace
{

// Reject 的原因映射成 errno，给 on_connect_failed。
int ErrnoFor(std::uint8_t reason) noexcept
{
    switch (static_cast<rudp::RejectReason>(reason))
    {
        case rudp::RejectReason::Ticket:
            return EACCES;
        case rudp::RejectReason::Busy:
            return EBUSY;
        case rudp::RejectReason::Filtered:
            return EPERM;
        case rudp::RejectReason::Version:
            return EPROTO;
        case rudp::RejectReason::Stopping:
            return ESHUTDOWN;
    }
    return ECONNREFUSED; // 不认识的原因码
}

static_assert(std::is_same_v<decltype(RUdpTicket::token), rudp::Token>);

} // namespace

// 状态：Idle → Handshaking → Connected → Closing → Idle。Closing 是 Stop 关掉了连接、但它的 OnClosed 还没回调的间隙：
// 这期间 Start 要等 OnClosed 之后再开始，否则旧连接的收尾会拆掉新连接的 socket 与定时器。
struct RUdpClient::Impl final : IDatagramHandler, RUdpConnection::Owner, IConnectionHandler
{
    enum class State : std::uint8_t
    {
        Idle,
        Handshaking,
        Connected,
        Closing,
    };

    Impl(EventLoopGroup& g, Endpoint ep, RUdpClientOptions o, IConnectionHandler& h, std::atomic<std::uint64_t>& hd)
        : group(g)
        , server(std::move(ep))
        , options(std::move(o))
        , handler(h)
        , handle(hd)
        , ticket(options.ticket)
    {
    }

    EventLoopGroup& group;
    Endpoint server;
    RUdpClientOptions options;
    IConnectionHandler& handler;
    std::atomic<std::uint64_t>& handle;

    std::mutex ticket_mu; // SetTicket（任意线程）与 Start（读）
    RUdpTicket ticket;

    EventLoop* loop = nullptr; // 第一次 Start 时选定，此后固定

    // 以下只在 loop 线程访问。
    State state = State::Idle;
    std::unique_ptr<UdpSocket> socket; // 关闭后对象留着（可能就在它的 OnDatagram 里），下次 Start 时才销毁
    RUdpConnection* conn = nullptr;
    RUdpTicket active; // 本次握手用的
    int last_error = 0;
    EventLoop::TimerId hello_timer = 0;
    EventLoop::TimerId deadline_timer = 0;
    EventLoop::TimerId tick_timer = 0;
    EventLoop::TimerId ping_timer = 0;
    EventLoop::HookId hook = 0;
    bool shutting_down = false; // 停机或析构：不再开始新的握手
    bool restart_pending = false;
    RUdpTicket restart_ticket;
    // 连接建立到它的 OnClosed 回调完成：析构要等它变回 false（回调里还在用 Impl）。
    std::atomic<bool> awaiting_closed{false};

    void StartOnLoop(const RUdpTicket& t);
    void StopOnLoop();
    void FailHandshake(int err);
    void CancelHandshakeTimers();
    void CancelRunTimers();
    void SendHello();
    void SendAbort();
    void OnHelloTimer();
    void OnAccepted();
    void ArmTick();
    void ArmPing();
    void HandleClosed(IConnection& c, CloseReason reason, int err);

    // IDatagramHandler
    void OnDatagram(const sockaddr_storage&, socklen_t, std::span<const std::byte> data) override;
    void OnSocketError(int err) override
    {
        if (state == State::Handshaking)
            last_error = err; // 握手期间记下来，超时时报告；连通之后的 ICMP 错误不关会话
    }
    // RUdpConnection::Owner
    void OnConnectionDetached(RUdpConnection& c) noexcept override
    {
        if (conn == &c)
            conn = nullptr; // Tick / 心跳不再碰它
    }
    // 客户端只有一条会话，定时器一直在跑，不需要活跃表。
    void OnConnectionWake(RUdpConnection&) noexcept override {}
    // IConnectionHandler：转发给业务，OnClosed 时先收尾
    void OnConnected(IConnection& c) noexcept override { handler.OnConnected(c); }
    void OnMessage(IConnection& c, std::span<const std::byte> payload) noexcept override
    {
        handler.OnMessage(c, payload);
    }
    void OnClosed(IConnection& c, CloseReason reason, int err) noexcept override { HandleClosed(c, reason, err); }
};

// ---- 握手 ----

void RUdpClient::Impl::StartOnLoop(const RUdpTicket& t)
{
    if (shutting_down)
        return;
    if (state == State::Closing)
    {
        restart_pending = true; // 旧连接的 OnClosed 之后再开始
        restart_ticket = t;
        return;
    }
    if (state != State::Idle)
        LOG_FATAL("RUdpClient::Start 在 Running 状态下重复调用 server={}", server.ToString());
    active = t;
    last_error = 0;
    socket.reset();
    state = State::Handshaking;
    if (const int e = UdpSocket::OpenClient(*loop, server, options.rudp, *this, socket); e != 0)
    {
        FailHandshake(e);
        return;
    }
    SendHello();
    hello_timer = loop->RunAfter(options.hello_interval,
                                 [this]
                                 {
                                     hello_timer = 0;
                                     OnHelloTimer();
                                 });
    deadline_timer = loop->RunAfter(options.connect_timeout,
                                    [this]
                                    {
                                        deadline_timer = 0;
                                        SendAbort();
                                        FailHandshake(last_error != 0 ? last_error : ETIMEDOUT);
                                    });
}

void RUdpClient::Impl::SendHello()
{
    const rudp::Control c = rudp::EncodeHello(active.conv, active.token);
    if (const int e = socket->Send({}, c.view(), nullptr, 0); e != 0)
        last_error = e; // 之前的 ICMP 不可达等；继续重试，超时时报告
}

// 放弃握手时告诉服务端：Accept 可能只是丢了或来晚了，服务端那边其实已经建好会话；不通知，
// 它就只能等空闲超时才回收（默认不开）。尽力而为，发一个；服务端没有这个会话时会当成陌生来源忽略。
void RUdpClient::Impl::SendAbort()
{
    if (socket && socket->open())
        (void)socket->Send(
            {}, rudp::EncodeClose(active.conv, static_cast<std::uint8_t>(CloseReason::LocalClose)).view(), nullptr, 0);
}

void RUdpClient::Impl::OnHelloTimer()
{
    if (state != State::Handshaking)
        return;
    SendHello();
    hello_timer = loop->RunAfter(options.hello_interval,
                                 [this]
                                 {
                                     hello_timer = 0;
                                     OnHelloTimer();
                                 });
}

void RUdpClient::Impl::CancelHandshakeTimers()
{
    if (hello_timer != 0)
        loop->Cancel(hello_timer);
    if (deadline_timer != 0)
        loop->Cancel(deadline_timer);
    hello_timer = deadline_timer = 0;
}

// 握手失败：关 socket、回到 Idle，再回调（回调里直接 Start() 是合法的：它只是投递）。
// 可能就在 socket 的 OnDatagram 里被调用，所以只关 fd，不销毁对象。
void RUdpClient::Impl::FailHandshake(int err)
{
    CancelHandshakeTimers();
    if (socket)
        socket->Close();
    state = State::Idle;
    LOG_WARN("rudp 握手 {} 失败 errno={}", server.ToString(), err);
    if (options.on_connect_failed)
        options.on_connect_failed(err, false);
}

void RUdpClient::Impl::OnDatagram(const sockaddr_storage&, socklen_t, std::span<const std::byte> data)
{
    LoopStats& st = loop->stats();
    const auto packet = rudp::Parse(data);
    if (!packet)
    {
        LoopStats::Bump(st.datagrams_dropped);
        return;
    }
    if (state == State::Handshaking)
    {
        if (packet->type == rudp::PacketType::Accept && packet->conv == active.conv &&
            packet->version == rudp::kVersion)
            OnAccepted();
        else if (packet->type == rudp::PacketType::Reject)
            FailHandshake(ErrnoFor(packet->reason));
        else
            LoopStats::Bump(st.datagrams_dropped); // 比 Accept 先到的数据：丢，KCP 会重传
        return;
    }
    if (state != State::Connected || conn == nullptr)
    {
        LoopStats::Bump(st.datagrams_dropped);
        return;
    }
    if (packet->type == rudp::PacketType::Accept)
        return; // Hello 重传与 Accept 交叉：多出来的 Accept，正常
    conn->OnPacket(*packet);
}

void RUdpClient::Impl::OnAccepted()
{
    CancelHandshakeTimers();
    if (!loop->table().TryReserve())
    {
        FailHandshake(ENOMEM); // 本 loop 的连接表满了
        return;
    }
    PeerInfo info;
    info.endpoint = server;
    RUdpConnection* c = RUdpConnection::Create(
        *loop, *socket, nullptr, 0, active.conv, std::move(info), *this, *this, options.connection, options.rudp);
    if (c == nullptr)
    {
        loop->table().Unreserve();
        FailHandshake(ENOMEM);
        return;
    }
    conn = c;
    state = State::Connected;
    awaiting_closed.store(true, std::memory_order_release);
    // 句柄先于 OnConnected 发布：OnConnected 里就能经 RUdpClient::Send 发数据。
    handle.store(c->handle().raw(), std::memory_order_release);
    ArmTick();
    ArmPing();
    c->Establish();
    LoopStats::Bump(loop->stats().connected);
}

// ---- 连通之后：定时推进与心跳 ----

void RUdpClient::Impl::ArmTick()
{
    tick_timer = loop->RunAfter(std::chrono::milliseconds(options.rudp.interval_ms),
                                [this]
                                {
                                    tick_timer = 0;
                                    if (conn == nullptr)
                                        return;
                                    conn->Tick(KcpNowMs());
                                    ArmTick();
                                });
}

void RUdpClient::Impl::ArmPing()
{
    if (options.ping_interval.count() <= 0)
        return;
    ping_timer = loop->RunAfter(options.ping_interval,
                                [this]
                                {
                                    ping_timer = 0;
                                    if (conn == nullptr || conn->closed())
                                        return;
                                    (void)socket->Send({}, rudp::EncodePing(active.conv).view(), nullptr, 0);
                                    ArmPing();
                                });
}

void RUdpClient::Impl::CancelRunTimers()
{
    if (tick_timer != 0)
        loop->Cancel(tick_timer);
    if (ping_timer != 0)
        loop->Cancel(ping_timer);
    tick_timer = ping_timer = 0;
}

// ---- 连接结束（NotifyClosed 里经转发器到这里）----

void RUdpClient::Impl::HandleClosed(IConnection& c, CloseReason reason, int err)
{
    handle.store(0, std::memory_order_release); // 先清句柄：回调里的 Send 得到 Closed
    conn = nullptr;
    CancelRunTimers();
    if (socket)
        socket->Close(); // 回收发生在 NotifyClosed 之后，连接此后不再用 socket
    state = State::Idle;
    const bool restart = std::exchange(restart_pending, false);
    handler.OnClosed(c, reason, err);
    if (restart && !shutting_down)
    {
        // 不在这里直接开始：旧连接的槽要到本轮回收才释放，连接表恰好满时 TryReserve 会误失败。
        // 定时器下一轮才触发，那时槽已经回收。
        const RUdpTicket t = restart_ticket;
        loop->RunAfter(std::chrono::milliseconds::zero(), [this, t] { StartOnLoop(t); });
    }
    // 清标志必须是这里最后一步：RUdpClient 的析构等的就是它，清了之后 Impl 随时可能被销毁；
    // 也必须在用户的 OnClosed 返回之后，否则析构会和还在执行的用户回调赛跑。
    awaiting_closed.store(false, std::memory_order_release);
}

void RUdpClient::Impl::StopOnLoop()
{
    restart_pending = false;
    switch (state)
    {
        case State::Idle:
        case State::Closing:
            return;
        case State::Handshaking:
            CancelHandshakeTimers();
            SendAbort();
            if (socket)
                socket->Close();
            state = State::Idle; // 自己叫停的，不回调 on_connect_failed
            return;
        case State::Connected:
            handle.store(0, std::memory_order_release); // 先清句柄：之后 RUdpClient::Send 得到 Closed
            state = State::Closing;
            if (conn != nullptr)
                conn->Close(); // 它的 OnClosed 随后回调，那时才回到 Idle
            return;
    }
}

// ---- 对外 ----

RUdpClient::RUdpClient(EventLoopGroup& group,
                       const RUdpEndpoint& server,
                       RUdpClientOptions options,
                       IConnectionHandler& handler)
    : server_(server.endpoint())
    , impl_(std::make_unique<Impl>(group, server.endpoint(), std::move(options), handler, handle_))
{
}

RUdpClient::~RUdpClient()
{
    Impl& m = *impl_;
    if (m.loop == nullptr || !m.group.running())
        return; // 没 Start 过；或 group 已 Shutdown，停机钩子已经处理过
    // 同步停：不然 Stop 投递的任务可能在本对象销毁之后才在 loop 上跑。
    (void)m.loop->CallSync(
        [&m]
        {
            m.shutting_down = true;
            m.StopOnLoop();
            if (m.hook != 0)
            {
                m.loop->RemoveShutdownHook(m.hook);
                m.hook = 0;
            }
            return 0;
        });
    // 被 Stop 关掉的连接要到本轮分发之后才回调 OnClosed（经转发器）：等它跑完再销毁。
    for (int i = 0; i < 5000 && m.awaiting_closed.load(std::memory_order_acquire); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

int RUdpClient::Start()
{
    Impl& m = *impl_;
    if (!m.group.running())
    {
        // 回调里 Start()（文档允许）可能恰好撞上停机：返回 ESHUTDOWN，不是编程错误。
        if (m.group.stopping())
            return ESHUTDOWN;
        LOG_FATAL("RUdpClient::Start 之前必须已 EventLoopGroup::Start() 成功 server={}", server_.ToString());
    }
    RUdpTicket t;
    {
        const std::lock_guard lk(m.ticket_mu);
        t = m.ticket;
    }
    if (!t.valid() || !m.options.rudp.Valid() || m.options.connect_timeout.count() <= 0 ||
        m.options.hello_interval.count() <= 0 || m.options.ping_interval.count() < 0)
        return EINVAL;
    if (m.loop == nullptr)
        m.loop = &GroupAccess::PickLoop(m.group);
    m.loop->Post(
        [&m, t]
        {
            if (m.hook == 0) // 停机时 loop 自己放弃握手、不再重开：group 不认识 RUdpClient，只认钩子
            {
                m.hook = m.loop->AddShutdownHook(
                    [&m]
                    {
                        m.hook = 0; // 钩子执行完就被清掉了
                        m.shutting_down = true;
                        m.StopOnLoop();
                    });
            }
            m.StartOnLoop(t);
        });
    return 0;
}

void RUdpClient::Stop()
{
    Impl& m = *impl_;
    if (m.loop == nullptr)
        return;
    m.loop->Post([&m] { m.StopOnLoop(); });
}

void RUdpClient::SetTicket(const RUdpTicket& ticket)
{
    const std::lock_guard lk(impl_->ticket_mu);
    impl_->ticket = ticket;
}

SendResult RUdpClient::Send(SharedBuffer payload)
{
    const ConnectionHandle h = handle();
    if (!h.valid())
        return SendResult::Closed;
    return h.Send(std::move(payload));
}

RUdpEndpoint RUdpClient::server() const { return *RUdpEndpoint::From(server_); }

} // namespace breeze
