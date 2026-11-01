#include "base/net/shm/shm_channel_server.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <optional>
#include <span>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "base/logger/logger.h"
#include "base/logger/logger_macros.h"
#include "base/net/loop/group_access.h"
#include "base/net/shm/shm_protocol.h"
#include "base/net/stream/internal/acceptor.h"
#include "base/net/stream/internal/fd_passing.h"
#include "base/net/stream/internal/stream_connection.h"
#include "base/net/uds/uds_socket.h"

namespace breeze
{

namespace
{

// 两个 fd 指向同一块内存（同一个 memfd）：Reattach 命中还留着的通道时，靠它确认 App 交来的是我们自己的那一块。
bool SameMemory(int a, int b)
{
    struct stat sa
    {
    };
    struct stat sb
    {
    };
    if (::fstat(a, &sa) != 0 || ::fstat(b, &sb) != 0)
        return false;
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

// 收到的控制消息的前缀必须与合法的头一致：垃圾数据在第一个不对的字节到达时就能识别，不用等攒够 16 字节。
bool ControlPrefixOk(std::span<const std::byte> got)
{
    const shm::wire::Header h = shm::wire::MakeHeader(shm::wire::MsgType::kControl);
    const std::size_t n = std::min(got.size(), sizeof(h));
    return std::memcmp(got.data(), &h, n) == 0;
}

} // namespace

class ShmServerSession;

// 仅 UDS 的对端的登记处，兼做它们的 IConnectionHandler（先转给业务，再释放名字与名额）。
// 连接归 EventLoop 的连接表持有，生命周期与 ShmChannelServer 不同：服务端析构之后连接可能还要等一轮才回收。
// 所以它由 shared_ptr 持有，每条连接的 UdsPeerContext 也持一份（keepalive）：连接销毁时它才可能销毁。
struct UdsRegistry final : IConnectionHandler
{
    struct Peer
    {
        std::uint64_t id;
        IConnection* conn; // 到 OnClosed 之前有效
    };
    IConnectionHandler* user = nullptr;
    std::unordered_map<std::string, Peer> peers; // 通道名 → 对端；只在 loop 线程上访问
    std::atomic<std::uint64_t> open{0};
    std::atomic<std::uint64_t> total{0};
    std::atomic<std::uint64_t> closed{0};
    std::atomic<std::uint64_t> downgraded{0};

    void OnConnected(IConnection& c) noexcept override { user->OnConnected(c); }
    void OnMessage(IConnection& c, std::span<const std::byte> payload) noexcept override { user->OnMessage(c, payload); }
    void OnClosed(IConnection& c, CloseReason reason, int err) noexcept override
    {
        user->OnClosed(c, reason, err);
        if (const UdsPeerContext* ctx = c.GetContext<UdsPeerContext>(); ctx != nullptr)
        {
            if (const auto it = peers.find(ctx->busid); it != peers.end() && it->second.id == ctx->channel_id)
                peers.erase(it);
        }
        closed.fetch_add(1, std::memory_order_relaxed);
        open.fetch_sub(1, std::memory_order_relaxed);
    }
};

struct ShmChannelServer::Impl
{
    Impl(EventLoopGroup& g, Endpoint ep, ShmServerOptions o, ShmChannelHandler& h, ShmUdsDataPath p)
        : group(g)
        , endpoint(std::move(ep))
        , options(std::move(o))
        , handler(h)
        , uds_path(p)
        , uds(std::make_shared<UdsRegistry>())
    {
        uds->user = p.handler;
    }
    ~Impl();

    EventLoopGroup& group;
    Endpoint endpoint;
    ShmServerOptions options;
    ShmChannelHandler& handler;
    ShmUdsDataPath uds_path;
    std::shared_ptr<UdsRegistry> uds;

    EventLoop* loop = nullptr; // Acceptor 与全部会话所在的 loop，Start 时选定
    std::unique_ptr<Acceptor> acceptor;
    EventLoop::HookId hook = 0;

    // 下面这些只在 loop 线程上访问。
    // 会话按单调递增的 id 索引：延后执行的拆除任务只带 id，查不到就是已经拆过了；
    // 用 id 而不是指针，是因为指针在地址被复用时会误拆别的会话。
    std::uint64_t next_id = 1;
    std::unordered_map<std::uint64_t, std::unique_ptr<ShmServerSession>> sessions;
    std::unordered_map<std::string, std::uint64_t> busids; // 通道名 → 已建立的会话 id（含保留中的）
    std::size_t pending = 0;                               // 还在握手的连接数
    std::uint64_t total_bytes = 0;                         // 已建立通道的共享内存总字节

    // 计数：loop 线程写，任意线程读。
    std::atomic<std::uint64_t> opened{0};
    std::atomic<std::uint64_t> closed{0};
    std::atomic<std::uint64_t> open_channels{0};
    std::atomic<std::uint64_t> rejected{0};
    std::atomic<std::uint64_t> handshake_timeouts{0};
    std::atomic<std::uint64_t> pending_overflow{0};
    std::atomic<std::uint64_t> protocol_errors{0};
    std::atomic<std::uint64_t> detached_channels{0};
    std::atomic<std::uint64_t> detached_total{0};
    std::atomic<std::uint64_t> resumed{0};
    std::atomic<std::uint64_t> reattached{0};
    std::atomic<std::uint64_t> retention_expired{0};
    std::atomic<std::uint64_t> discarded{0};
    std::atomic<std::uint64_t> byes{0};
    std::atomic<std::uint64_t> inflight_skipped{0};
    std::atomic<std::uint64_t> rx_bytes_discarded{0};

    // 全部通道数（共享内存的含保留中的，加仅 UDS 的）：配额按它算。
    std::uint64_t TotalChannels() const noexcept
    {
        return open_channels.load(std::memory_order_relaxed) + uds->open.load(std::memory_order_relaxed);
    }
    bool BusIdInUse(const std::string& busid) const { return busids.contains(busid) || uds->peers.contains(busid); }

    int StartOnLoop();
    void StopOnLoop();
    void OnAccept(UniqueFd fd, std::optional<Endpoint> peer);
    void OnHandshakeTimeout(std::uint64_t id);
    void OnRetentionTimer(std::uint64_t id);
    ShmServerSession* Find(std::uint64_t id);
    ShmServerSession* FindByBusId(const std::string& busid);
    // 通道名只在"建立"到"开始关闭"之间归它：开始关闭就放出来，同名的新通道不用等延后的拆除。
    void ReleaseBusId(const std::string& busid, std::uint64_t id);
    // 会话在自己的回调栈里请求关闭时，拆除延后到本轮事件分发之后（任务阶段）：
    // 同一批 epoll 事件里还可能有它的别的 fd 的事件，对象必须活到那之后。
    void PostTeardown(std::uint64_t id);
    void PostReadable(std::uint64_t id);
    void Teardown(std::uint64_t id);
};

// 一个连接的完整生命周期：握手 → 通道（Open ⇄ Detached）→ 关闭。握手阶段是 IPollable（UDS fd），
// 建立之后还多一个 IPollable（Tx 环的 eventfd）。同时是回调里用的 ShmServerChannel。
//
// 一条通道对应一个 ShmServerSession 对象，跨保留与恢复不换：App 重新来握手时，新来的连接（一个还在握手的会话）
// 查到它，把 UDS 交给它（ResumeOld），握手会话随即销毁。
class ShmServerSession final : public ShmServerChannel, private IPollable
{
public:
    ShmServerSession(ShmChannelServer::Impl& impl, std::uint64_t id, UniqueFd sock)
        : impl_(impl)
        , id_(id)
        , sock_(std::move(sock))
        , tx_wake_(*this)
    {
    }

    // ---- ShmServerChannel ----
    std::uint64_t id() const noexcept override { return id_; }
    const std::string& busid() const noexcept override { return busid_; }
    const PeerCredentials& peer() const noexcept override { return peer_; }
    shm::ShmChannel& shm() noexcept override { return *chan_; }
    shm::RingConsumer& tx() noexcept override { return tx_; }
    shm::RingProducer& rx() noexcept override { return rx_; }
    std::uint64_t app_epoch() const noexcept override { return app_epoch_; }
    std::uint64_t agent_epoch() const noexcept override { return agent_epoch_; }
    bool detached() const noexcept override { return state_ == State::Detached; }
    bool reattached() const noexcept override { return reattached_; }
    void Close() noexcept override { BeginClose(ShmCloseReason::LocalClose); }

    // ---- Impl 用 ----
    // 登记 UDS fd 与握手定时器。返回 errno。
    int Start(PeerCredentials peer);
    // 把自己从 loop 上摘下来（fd、定时器），并记下原因。已经在关闭则返回 false（保留第一个原因）。
    bool Detach(ShmCloseReason reason);
    // 摘下来，并延后拆除。回调栈里关闭一律走这条。
    void BeginClose(ShmCloseReason reason);
    void RunReadable();
    void OnHandshakeTimeout();
    void OnRetentionTimer();

    bool opened() const noexcept { return opened_; }
    bool handshaking() const noexcept { return state_ == State::Handshaking; }
    ShmCloseReason close_reason() const noexcept { return close_reason_; }
    const std::string& busid_for_map() const noexcept { return busid_; }
    std::uint64_t memory_bytes() const noexcept { return memory_bytes_; }
    void ReleasePendingSlot();
    void SetReadableScheduled(bool v) noexcept { readable_scheduled_ = v; }
    // 能收回调的状态：App 在（Open）或不在但队列保留着（Detached）。
    bool live() const noexcept { return state_ == State::Open || state_ == State::Detached; }

private:
    enum class State : std::uint8_t
    {
        Handshaking,
        Open,     // Attached：App 在线
        Detached, // UDS 断了：保留中，或者在排空 Tx 之后就销毁（drain_then_close_）
        Closing,
    };

    // Tx 环的 eventfd：App 在消费者睡着时 Commit 会敲它。
    class TxWake final : public IPollable
    {
    public:
        explicit TxWake(ShmServerSession& s)
            : s_(s)
        {
        }
        void OnEvents(std::uint32_t) override { s_.OnTxEvents(); }

    private:
        ShmServerSession& s_;
    };

    void OnEvents(std::uint32_t events) override;
    void OnTxEvents();
    void ReadHello();
    void FinishHandshake(const shm::wire::Hello& hello);
    void HandleAttach(const shm::wire::Hello& hello, bool may_downgrade);
    void HandleReattach(const shm::wire::Hello& hello);
    // 仅 UDS：握手之后把这条 UDS 交给 StreamConnection。downgraded = 本来要共享内存、给不了退化过来的；
    // admitted = 准入回调已经调过了。
    void HandleUdsOnly(const shm::wire::Hello& hello, bool downgraded, bool admitted);
    // 共享内存给不了（内存配额满、创建 / 登记失败）：App 说过可以退化就改走仅 UDS，否则拒绝。
    void FailOrDowngrade(int err, const shm::wire::Hello& hello, bool may_downgrade);
    void Reject(int err);
    void DrainOpenSocket(std::uint32_t events);
    bool HandleControl(const shm::wire::Control& c);
    ShmAdmitRequest MakeAdmit(const shm::wire::Hello& hello, bool reattach, bool resuming, bool discard) const;
    shm::wire::Reply BuildReply(bool resumed, std::uint64_t app_epoch) const;
    // 通道已经建好并发出 Accept：登记配额与映射，回调 OnOpened，跑第一次消费循环。
    void Establish(bool reattached);
    // 把自己（一个握手会话）的 UDS 交给保留着的 old，然后自己销毁。
    void ResumeOld(ShmServerSession& old, bool bump_app_epoch, bool send_fds);

    // ---- 作为"保留着的通道"被接手 / 判断 ----
    UniqueFd ReleaseSocket();
    // 返回 errno；失败时什么都没改变（仍在保留中）。
    int Resume(UniqueFd sock, const PeerCredentials& peer, bool bump_app_epoch, bool send_fds);
    // 旧连接若已经断了，现在就转入保留，不用等 EOF 事件到达（App 快速重启时 Hello 可能先于 EOF 被处理）。
    void ProbeAlive();
    bool resumable() const noexcept { return state_ == State::Detached && !drain_then_close_; }
    bool CapacityMatches(const shm::wire::Hello& hello) const noexcept;
    // UDS 上读到 EOF / 被重置：App 没了。转入保留（或排空之后销毁）。
    void OnPeerGone();
    void DropSocket();

    ShmChannelServer::Impl& impl_;
    const std::uint64_t id_;
    UniqueFd sock_;
    State state_ = State::Handshaking;
    bool sock_registered_ = false;
    bool tx_registered_ = false;
    bool pending_counted_ = false;
    bool opened_ = false;
    bool reattached_ = false;
    bool readable_scheduled_ = false;
    bool in_retention_ = false;      // 计入了 detached_channels
    bool drain_then_close_ = false;  // Detached 且不保留：Tx 排空（或排空超时）就销毁
    bool bye_ = false;               // 收到过 Bye
    ShmCloseReason drain_reason_ = ShmCloseReason::PeerClosed;
    ShmCloseReason close_reason_ = ShmCloseReason::ProtocolError;
    std::optional<std::chrono::milliseconds> prepared_retain_; // PrepareRestart 设置的，只管下一次断开
    EventLoop::TimerId timer_ = 0;           // 握手超时
    EventLoop::TimerId retention_timer_ = 0; // 保留 / 排空超时
    PeerCredentials peer_{};

    // 握手：定长的 Hello，收齐才处理；Reattach 的 fd 随它到达（至多 4 个槽位：多于 3 个就是协议错误）。
    std::array<std::byte, sizeof(shm::wire::Hello)> hello_buf_{};
    std::size_t hello_len_ = 0;
    std::array<UniqueFd, 4> hello_fds_;
    std::size_t hello_nfds_ = 0;

    // 握手之后：UDS 上只有定长的 Control。
    std::array<std::byte, sizeof(shm::wire::Control)> ctl_buf_{};
    std::size_t ctl_len_ = 0;

    // 建立之后。
    std::string busid_;
    std::optional<shm::ShmChannel> chan_;
    shm::RingConsumer tx_;
    shm::RingProducer rx_;
    std::uint64_t app_epoch_ = 0;
    std::uint64_t agent_epoch_ = 0;
    std::uint64_t memory_bytes_ = 0;
    TxWake tx_wake_;
};

// ---- Impl ----

ShmChannelServer::Impl::~Impl() = default;

ShmServerSession* ShmChannelServer::Impl::Find(std::uint64_t id)
{
    const auto it = sessions.find(id);
    return it == sessions.end() ? nullptr : it->second.get();
}

ShmServerSession* ShmChannelServer::Impl::FindByBusId(const std::string& busid)
{
    const auto it = busids.find(busid);
    return it == busids.end() ? nullptr : Find(it->second);
}

void ShmChannelServer::Impl::ReleaseBusId(const std::string& busid, std::uint64_t id)
{
    if (const auto b = busids.find(busid); b != busids.end() && b->second == id)
        busids.erase(b);
}

void ShmChannelServer::Impl::PostTeardown(std::uint64_t id)
{
    loop->Post([this, id] { Teardown(id); });
}

void ShmChannelServer::Impl::PostReadable(std::uint64_t id)
{
    loop->Post(
        [this, id]
        {
            ShmServerSession* s = Find(id);
            if (s == nullptr || !s->live())
                return;
            s->SetReadableScheduled(false);
            s->RunReadable();
        });
}

void ShmChannelServer::Impl::Teardown(std::uint64_t id)
{
    const auto it = sessions.find(id);
    if (it == sessions.end())
        return; // 已经拆过了（比如停机时同步拆掉了，这是之前投递的任务）
    std::unique_ptr<ShmServerSession> s = std::move(it->second);
    sessions.erase(it);
    s->ReleasePendingSlot();
    if (!s->opened())
        return; // 通道从未建立：没有回调、没有占用任何配额
    ReleaseBusId(s->busid_for_map(), id);
    total_bytes -= s->memory_bytes();
    open_channels.fetch_sub(1, std::memory_order_relaxed);
    closed.fetch_add(1, std::memory_order_relaxed);
    handler.OnClosed(*s, s->close_reason());
    // s 在这里销毁：fd 关闭、映射解除。
}

int ShmChannelServer::Impl::StartOnLoop()
{
    if (const int e = acceptor->Listen(options.backlog); e != 0)
        return e;
    // 停机时 loop 自己停掉 accept 并关闭全部通道：group 不认识这个服务端，只认钩子。
    hook = loop->AddShutdownHook([this] { StopOnLoop(); });
    return 0;
}

void ShmChannelServer::Impl::StopOnLoop()
{
    if (acceptor)
        acceptor->Stop();
    if (hook != 0)
    {
        loop->RemoveShutdownHook(hook);
        hook = 0;
    }
    // 仅 UDS 的连接：关掉（OnClosed 在本轮分发之后回调，由登记处释放名额）。
    for (auto& [busid, p] : uds->peers)
        p.conn->Close();
    // 此刻在任务阶段（本轮 epoll 事件已经分发完），可以同步拆除，不用延后。保留中的通道也一起销毁。
    std::vector<std::uint64_t> ids;
    ids.reserve(sessions.size());
    for (const auto& [id, s] : sessions)
        ids.push_back(id);
    for (const std::uint64_t id : ids)
    {
        if (ShmServerSession* s = Find(id))
            s->Detach(ShmCloseReason::ServerStopped);
        Teardown(id);
    }
}

void ShmChannelServer::Impl::OnAccept(UniqueFd fd, std::optional<Endpoint> /*peer*/)
{
    if (pending >= options.max_pending_handshakes)
    {
        pending_overflow.fetch_add(1, std::memory_order_relaxed);
        return; // fd 随之关闭
    }
    PeerCredentials cred;
    if (const int e = uds::GetPeerCredentials(fd.get(), cred); e != 0)
    {
        LOG_WARN("shm: 取 SO_PEERCRED 失败 errno={}，断开", e);
        return;
    }
    const std::uint64_t id = next_id++;
    auto s = std::make_unique<ShmServerSession>(*this, id, std::move(fd));
    ShmServerSession* raw = s.get();
    sessions.emplace(id, std::move(s));
    if (const int e = raw->Start(cred); e != 0)
    {
        LOG_WARN("shm: 登记新连接失败 errno={}，断开", e);
        Teardown(id); // 还没有任何东西指向它（Start 失败时已回滚），直接拆
    }
}

void ShmChannelServer::Impl::OnHandshakeTimeout(std::uint64_t id)
{
    if (ShmServerSession* s = Find(id))
        s->OnHandshakeTimeout();
}

void ShmChannelServer::Impl::OnRetentionTimer(std::uint64_t id)
{
    if (ShmServerSession* s = Find(id))
        s->OnRetentionTimer();
}

// ---- ShmServerSession：生命周期 ----

int ShmServerSession::Start(PeerCredentials peer)
{
    peer_ = peer;
    if (const int e = impl_.loop->Add(this, sock_.get(), EPOLLIN | EPOLLRDHUP); e != 0)
        return e;
    sock_registered_ = true;
    ++impl_.pending;
    pending_counted_ = true;
    const std::uint64_t id = id_;
    timer_ = impl_.loop->RunAfter(impl_.options.handshake_timeout, [this, id] { impl_.OnHandshakeTimeout(id); });
    return 0;
}

void ShmServerSession::ReleasePendingSlot()
{
    if (pending_counted_)
    {
        --impl_.pending;
        pending_counted_ = false;
    }
}

bool ShmServerSession::Detach(ShmCloseReason reason)
{
    if (state_ == State::Closing)
        return false;
    state_ = State::Closing;
    close_reason_ = reason;
    if (timer_ != 0)
    {
        impl_.loop->Cancel(timer_);
        timer_ = 0;
    }
    if (retention_timer_ != 0)
    {
        impl_.loop->Cancel(retention_timer_);
        retention_timer_ = 0;
    }
    if (in_retention_)
    {
        impl_.detached_channels.fetch_sub(1, std::memory_order_relaxed);
        in_retention_ = false;
    }
    if (sock_registered_)
    {
        impl_.loop->Del(sock_.get());
        sock_registered_ = false;
    }
    if (tx_registered_)
    {
        impl_.loop->Del(chan_->tx_eventfd());
        tx_registered_ = false;
    }
    if (opened_)
        impl_.ReleaseBusId(busid_, id_);
    return true;
}

void ShmServerSession::BeginClose(ShmCloseReason reason)
{
    if (Detach(reason))
        impl_.PostTeardown(id_);
}

void ShmServerSession::OnHandshakeTimeout()
{
    timer_ = 0; // 已经触发，不用再取消
    if (state_ != State::Handshaking)
        return;
    impl_.handshake_timeouts.fetch_add(1, std::memory_order_relaxed);
    BeginClose(ShmCloseReason::HandshakeTimeout);
}

void ShmServerSession::OnRetentionTimer()
{
    retention_timer_ = 0; // 已经触发，不用再取消
    if (state_ != State::Detached)
        return;
    if (drain_then_close_)
    {
        BeginClose(drain_reason_); // 排空超时：不再等
        return;
    }
    impl_.retention_expired.fetch_add(1, std::memory_order_relaxed);
    // 没被读走的字节只用来记指标：对端写的 read_idx / write_idx 不可信，占用超过容量就不记。
    const std::uint64_t w = chan_->rx_control().write_idx.load(std::memory_order_acquire);
    const std::uint64_t r = chan_->rx_control().read_idx.load(std::memory_order_acquire);
    if (w - r <= chan_->layout().rx.capacity)
        impl_.rx_bytes_discarded.fetch_add(w - r, std::memory_order_relaxed);
    BeginClose(ShmCloseReason::RetentionExpired);
}

void ShmServerSession::DropSocket()
{
    if (sock_registered_)
    {
        impl_.loop->Del(sock_.get());
        sock_registered_ = false;
    }
    sock_.reset(); // Del 在先：fd 不再在 epoll 里，当场关闭是安全的（对象本身仍然活着）
}

UniqueFd ShmServerSession::ReleaseSocket()
{
    if (timer_ != 0)
    {
        impl_.loop->Cancel(timer_);
        timer_ = 0;
    }
    if (sock_registered_)
    {
        impl_.loop->Del(sock_.get());
        sock_registered_ = false;
    }
    return std::move(sock_);
}

// ---- ShmServerSession：保留与恢复 ----

void ShmServerSession::OnPeerGone()
{
    if (state_ != State::Open)
        return;
    DropSocket();
    std::chrono::milliseconds retain = impl_.options.retain_default;
    if (prepared_retain_)
        retain = *prepared_retain_;
    if (bye_)
        retain = std::chrono::milliseconds::zero();
    prepared_retain_.reset();
    state_ = State::Detached;
    const std::uint64_t id = id_;
    if (retain.count() <= 0)
    {
        // 不保留：把已经提交的遗言排空就销毁；对端要是不老实还在写，排空超时兜底。
        drain_then_close_ = true;
        drain_reason_ = bye_ ? ShmCloseReason::PeerBye : ShmCloseReason::PeerClosed;
        retention_timer_ =
            impl_.loop->RunAfter(impl_.options.drain_timeout, [this, id] { impl_.OnRetentionTimer(id); });
    }
    else
    {
        in_retention_ = true;
        impl_.detached_channels.fetch_add(1, std::memory_order_relaxed);
        impl_.detached_total.fetch_add(1, std::memory_order_relaxed);
        retention_timer_ = impl_.loop->RunAfter(retain, [this, id] { impl_.OnRetentionTimer(id); });
        LOG_DEBUG("shm: 通道进入保留 busid={} 保留={}ms", busid_, retain.count());
        impl_.handler.OnDetached(*this, retain);
    }
    // 遗言：Tx 里已经提交的记录照常处理完。回调里要是销毁了通道，RunReadable 里会发现。
    RunReadable();
}

void ShmServerSession::ProbeAlive()
{
    if (state_ == State::Open)
        DrainOpenSocket(0);
}

bool ShmServerSession::CapacityMatches(const shm::wire::Hello& h) const noexcept
{
    const shm::ChannelLayout& l = chan_->layout();
    return (h.tx_capacity == 0 || h.tx_capacity == l.tx.capacity) &&
           (h.rx_capacity == 0 || h.rx_capacity == l.rx.capacity) &&
           (h.tx_max_payload == 0 || h.tx_max_payload == l.tx.max_payload) &&
           (h.rx_max_payload == 0 || h.rx_max_payload == l.rx.max_payload);
}

int ShmServerSession::Resume(UniqueFd sock, const PeerCredentials& peer, bool bump_app_epoch, bool send_fds)
{
    // 先做所有可能失败的事，成功了再改状态：失败时通道仍在保留中，保留计时器照走。
    if (const int e = impl_.loop->Add(this, sock.get(), EPOLLIN | EPOLLRDHUP); e != 0)
        return e;
    // epoch 要在发 Reply 之前加：客户端一收到回复就会拿共享内存头里的值对账（Superseded）。
    // 之后发送失败也不回退：计数单调，跳过一个值无害，下一次恢复再加。
    const std::uint64_t new_epoch = bump_app_epoch ? chan_->BumpAppEpoch() : chan_->header().app_epoch.load(std::memory_order_acquire);
    const shm::wire::Reply reply = BuildReply(true, new_epoch);
    const std::array<int, 3> fds{chan_->memfd(), chan_->tx_eventfd(), chan_->rx_eventfd()};
    if (const int e = sock::SendWithFds(
            sock.get(), std::as_bytes(std::span(&reply, 1)), send_fds ? std::span<const int>(fds) : std::span<const int>());
        e != 0)
    {
        impl_.loop->Del(sock.get());
        LOG_DEBUG("shm: 恢复时发送 Accept 失败 busid={} errno={}", busid_, e);
        return e;
    }

    if (retention_timer_ != 0)
    {
        impl_.loop->Cancel(retention_timer_);
        retention_timer_ = 0;
    }
    if (in_retention_)
    {
        impl_.detached_channels.fetch_sub(1, std::memory_order_relaxed);
        in_retention_ = false;
    }
    sock_ = std::move(sock);
    sock_registered_ = true;
    peer_ = peer;
    state_ = State::Open;
    bye_ = false;
    prepared_retain_.reset();
    ctl_len_ = 0;
    app_epoch_ = new_epoch;
    LOG_DEBUG("shm: 通道恢复 busid={} pid={} app_epoch={}", busid_, peer_.pid, app_epoch_);
    impl_.handler.OnResumed(*this);
    RunReadable();
    return 0;
}

void ShmServerSession::ResumeOld(ShmServerSession& old, bool bump_app_epoch, bool send_fds)
{
    UniqueFd sock = ReleaseSocket();
    const PeerCredentials peer = peer_;
    // 握手会话的使命到此为止：从未建立，没有回调，也不占配额。
    BeginClose(ShmCloseReason::LocalClose);
    if (const int e = old.Resume(std::move(sock), peer, bump_app_epoch, send_fds); e != 0)
    {
        // 通道仍在保留中；对端拿不到 Reply（连接随 sock 关闭），它会看到 EOF 后重试。
        LOG_WARN("shm: 恢复保留的通道失败 busid={} errno={}", old.busid_, e);
        return;
    }
    (send_fds ? impl_.resumed : impl_.reattached).fetch_add(1, std::memory_order_relaxed);
}

// ---- ShmServerSession：握手 ----

void ShmServerSession::OnEvents(std::uint32_t events)
{
    if (state_ == State::Closing)
        return; // 同一批事件里先到的回调已经请求关闭：对象还活着，但不再处理
    if (state_ == State::Handshaking)
    {
        if ((events & EPOLLERR) != 0)
        {
            BeginClose(ShmCloseReason::IoError);
            return;
        }
        ReadHello(); // EPOLLIN / EPOLLHUP / EPOLLRDHUP 都靠读来判定（读到 0 就是 EOF）
        return;
    }
    if (state_ == State::Open)
        DrainOpenSocket(events);
}

// 握手阶段：只读还差的那么多字节（不多读，也就不会吞掉握手之后的数据），收齐就处理。
// 用 recvmsg 收：Reattach 的 Hello 带 fd。Attach 带了 fd 就是协议错误；超过 3 个 fd 的整条当协议错误，已收到的随 UniqueFd 关闭。
void ShmServerSession::ReadHello()
{
    while (hello_len_ < hello_buf_.size())
    {
        std::array<UniqueFd, 4> chunk;
        std::size_t n = 0;
        std::size_t nf = 0;
        const int e = sock::RecvWithFds(sock_.get(), std::span(hello_buf_).subspan(hello_len_), n, chunk, nf);
        if (e == EAGAIN || e == EWOULDBLOCK)
            return;
        if (e == EMSGSIZE) // 控制缓冲被截断：对端塞的 fd 比槽位多
        {
            impl_.protocol_errors.fetch_add(1, std::memory_order_relaxed);
            BeginClose(ShmCloseReason::ProtocolError);
            return;
        }
        if (e != 0)
        {
            BeginClose(ShmCloseReason::IoError);
            return;
        }
        if (n == 0)
        {
            BeginClose(ShmCloseReason::PeerClosed); // 没发完 Hello 就走了
            return;
        }
        hello_len_ += n;
        for (std::size_t i = 0; i < nf; ++i)
        {
            if (hello_nfds_ >= hello_fds_.size())
            {
                impl_.protocol_errors.fetch_add(1, std::memory_order_relaxed);
                BeginClose(ShmCloseReason::ProtocolError);
                return;
            }
            hello_fds_[hello_nfds_++] = std::move(chunk[i]);
        }
    }
    shm::wire::Hello hello;
    std::memcpy(&hello, hello_buf_.data(), sizeof(hello)); // 先拷到本地再校验
    FinishHandshake(hello);
    for (UniqueFd& f : hello_fds_) // 该用的已经接管走了，剩下的（被拒绝的）不要留着占 fd 表
        f.reset();
}

void ShmServerSession::Reject(int err)
{
    shm::wire::Reply r{};
    r.hdr = shm::wire::MakeHeader(shm::wire::MsgType::kReply);
    r.result = err;
    // 先计数再回复：对端收到 Reject 之后马上去看统计，不能读到还没更新的值。
    impl_.rejected.fetch_add(1, std::memory_order_relaxed);
    // 回复失败也无所谓（对端可能已经走了）：反正马上关。
    (void)sock::SendWithFds(sock_.get(), std::as_bytes(std::span(&r, 1)), {});
    LOG_DEBUG("shm: 拒绝握手 busid={} errno={}", busid_, err);
    BeginClose(ShmCloseReason::ProtocolError);
}

ShmAdmitRequest ShmServerSession::MakeAdmit(const shm::wire::Hello& hello, bool reattach, bool resuming, bool discard) const
{
    ShmAdmitRequest req{busid_, peer_, hello.pid, hello.tx_capacity, hello.rx_capacity};
    req.transport = static_cast<shm::wire::Transport>(hello.transport);
    req.reattach = reattach;
    req.resuming = resuming;
    req.discard_retained = discard;
    return req;
}

shm::wire::Reply ShmServerSession::BuildReply(bool resumed, std::uint64_t app_epoch) const
{
    using namespace shm;
    const ChannelLayout& l = chan_->layout();
    wire::Reply reply{};
    reply.hdr = wire::MakeHeader(wire::MsgType::kReply);
    reply.result = 0;
    reply.resumed = resumed ? 1 : 0;
    reply.app_epoch = app_epoch;
    reply.agent_epoch = agent_epoch_;
    reply.total_size = l.total_size;
    reply.tx = {l.tx.capacity, l.tx.max_payload, 0};
    reply.rx = {l.rx.capacity, l.rx.max_payload, 0};
    reply.transport = static_cast<std::uint32_t>(wire::ChosenTransport::kShm);
    return reply;
}

void ShmServerSession::FinishHandshake(const shm::wire::Hello& hello)
{
    using namespace shm;
    if (const int e = wire::CheckHello(hello); e != 0)
    {
        Reject(e);
        return;
    }
    const std::string_view name = *wire::GetBusId(hello); // CheckHello 已经保证合法
    busid_.assign(name);
    if (hello.attach_type == static_cast<std::uint32_t>(wire::AttachType::kReattach))
    {
        HandleReattach(hello); // CheckHello 保证了 Reattach 一定要共享内存
        return;
    }
    // 协商传输方式：App 说了算要什么，Agent 说了算给不给。
    //   UdsOnly       只用 UDS
    //   ShmPreferred  要共享内存；Agent 不提供 / 给不了（内存配额满、创建失败）时退化成仅 UDS
    //   ShmRequired   要共享内存；给不了就拒绝
    // 只有"可用性"类的失败才退化；App 自己的问题（容量不合法、准入被拒、名字被占）照常拒绝，不拿退化掩盖。
    const auto tr = static_cast<wire::Transport>(hello.transport);
    const bool uds_ready = impl_.uds_path.handler != nullptr && impl_.uds_path.factory != nullptr;
    if (tr == wire::Transport::kUdsOnly)
    {
        HandleUdsOnly(hello, false, false);
        return;
    }
    const bool may_downgrade = tr == wire::Transport::kShmPreferred && uds_ready;
    if (!impl_.options.allow_shm)
    {
        if (may_downgrade)
            HandleUdsOnly(hello, true, false);
        else
            Reject(ENOTSUP);
        return;
    }
    HandleAttach(hello, may_downgrade);
}

namespace
{
// 共享内存"暂时给不了"的错误：内存 / fd 不够，或这个环境不让建 memfd。
bool IsShmUnavailable(int e) noexcept
{
    return e == ENOSPC || e == ENOMEM || e == EMFILE || e == ENFILE || e == ENOSYS || e == EPERM;
}
} // namespace

void ShmServerSession::FailOrDowngrade(int err, const shm::wire::Hello& hello, bool may_downgrade)
{
    if (may_downgrade && IsShmUnavailable(err))
    {
        LOG_DEBUG("shm: 共享内存给不了（errno={}），busid={} 退化成仅 UDS", err, busid_);
        HandleUdsOnly(hello, true, true);
        return;
    }
    Reject(err);
}

// 仅 UDS：回一条不带 fd 的 Accept，然后把 UDS 交给 StreamConnection。
void ShmServerSession::HandleUdsOnly(const shm::wire::Hello& hello, bool downgraded, bool admitted)
{
    using namespace shm;
    if (hello_nfds_ != 0)
    {
        Reject(EPROTO);
        return;
    }
    const ShmUdsDataPath& path = impl_.uds_path;
    if (path.handler == nullptr || path.factory == nullptr)
    {
        Reject(ENOTSUP);
        return;
    }
    const bool discard = (hello.flags & wire::kFlagDiscardRetained) != 0;
    // 通道名在共享内存的与仅 UDS 的之间统一：保留着的共享内存通道的队列没法交给一个仅 UDS 的 App，
    // 要换传输方式得带 discard_retained 明说（丢掉旧队列，只还给同一个用户）。
    ShmServerSession* discard_old = nullptr;
    if (ShmServerSession* old = impl_.FindByBusId(busid_); old != nullptr)
    {
        old->ProbeAlive();
        if (!old->resumable() || !discard)
        {
            Reject(EBUSY);
            return;
        }
        if (old->peer_.uid != peer_.uid)
        {
            Reject(EACCES);
            return;
        }
        discard_old = old;
    }
    else if (impl_.uds->peers.contains(busid_))
    {
        Reject(EBUSY);
        return;
    }
    if (impl_.TotalChannels() - (discard_old != nullptr ? 1 : 0) >= impl_.options.max_channels)
    {
        Reject(ENOSPC);
        return;
    }
    if (!admitted)
    {
        if (const int e = impl_.handler.OnAdmit(MakeAdmit(hello, false, false, discard)); e != 0)
        {
            Reject(e);
            return;
        }
    }
    EventLoop& loop = *impl_.loop;
    if (!loop.table().TryReserve()) // 连接表满了
    {
        Reject(ENOSPC);
        return;
    }
    wire::Reply reply{};
    reply.hdr = wire::MakeHeader(wire::MsgType::kReply);
    reply.result = 0;
    reply.transport = static_cast<std::uint32_t>(wire::ChosenTransport::kUds);
    if (const int e = sock::SendWithFds(sock_.get(), std::as_bytes(std::span(&reply, 1)), {}); e != 0)
    {
        loop.table().Unreserve();
        LOG_DEBUG("shm: 发送 Accept 失败 busid={} errno={}", busid_, e);
        BeginClose(ShmCloseReason::IoError);
        return;
    }
    if (discard_old != nullptr)
    {
        impl_.discarded.fetch_add(1, std::memory_order_relaxed);
        discard_old->BeginClose(ShmCloseReason::Discarded);
    }

    // 交接：UDS 从本会话摘下来给 StreamConnection；本会话（从未建立：没有 OnOpened、不占配额）随即销毁。
    UniqueFd fd = ReleaseSocket();
    const PeerCredentials peer = peer_;
    const std::string busid = busid_;
    const std::uint64_t id = id_;
    BeginClose(ShmCloseReason::LocalClose);

    PeerInfo info;
    info.credentials = peer;
    StreamConnection* conn =
        StreamConnection::Create(loop, std::move(fd), std::move(info), *impl_.uds, path.factory->Create(), path.connection);
    auto ctx = std::make_unique<UdsPeerContext>();
    ctx->busid = busid;
    ctx->cred = peer;
    ctx->channel_id = id;
    ctx->keepalive = impl_.uds;
    conn->SetContext(std::move(ctx)); // 在 Establish 之前：OnConnected 里就能取到
    impl_.uds->peers.emplace(busid, UdsRegistry::Peer{id, conn});
    impl_.uds->open.fetch_add(1, std::memory_order_relaxed);
    impl_.uds->total.fetch_add(1, std::memory_order_relaxed);
    if (downgraded)
        impl_.uds->downgraded.fetch_add(1, std::memory_order_relaxed);
    LOG_DEBUG("shm: 仅 UDS 通道建立 busid={} pid={} uid={} 退化={}", busid, peer.pid, peer.uid, downgraded);
    if (!conn->Establish())
    {
        // 登记 epoll 失败：连接已被标记关闭、没有任何回调（不会走到 OnClosed），自己把登记撤掉。
        impl_.uds->peers.erase(busid);
        impl_.uds->open.fetch_sub(1, std::memory_order_relaxed);
        impl_.uds->total.fetch_sub(1, std::memory_order_relaxed);
        if (downgraded)
            impl_.uds->downgraded.fetch_sub(1, std::memory_order_relaxed);
    }
}

// App 来要通道：新建，或接手保留着的那一条。
void ShmServerSession::HandleAttach(const shm::wire::Hello& hello, bool may_downgrade)
{
    using namespace shm;
    if (hello_nfds_ != 0) // Attach 不该带 fd
    {
        Reject(EPROTO);
        return;
    }
    const bool discard = (hello.flags & wire::kFlagDiscardRetained) != 0;

    // 配额与唯一性，都在创建共享内存之前：拒绝的代价只是一次 Reply。
    ShmServerSession* discard_old = nullptr;
    bool admitted = false;
    if (ShmServerSession* old = impl_.FindByBusId(busid_); old != nullptr)
    {
        old->ProbeAlive(); // 旧连接要是已经断了，现在就转入保留
        if (!old->resumable())
        {
            Reject(EBUSY);
            return;
        }
        if (old->peer_.uid != peer_.uid) // 保留着的队列只还给同一个用户
        {
            Reject(EACCES);
            return;
        }
        if (const int e = impl_.handler.OnAdmit(MakeAdmit(hello, false, !discard, discard)); e != 0)
        {
            Reject(e);
            return;
        }
        admitted = true;
        if (!discard)
        {
            if (!old->CapacityMatches(hello)) // 沿用原容量；想换大小要带 discard_retained
            {
                Reject(EINVAL);
                return;
            }
            ResumeOld(*old, true, true);
            return;
        }
        discard_old = old; // 新通道建好、过了配额再丢旧的：参数不合法时不能白白毁掉保留着的队列
    }
    else if (impl_.uds->peers.contains(busid_)) // 这个名字被一个仅 UDS 的对端占着
    {
        Reject(EBUSY);
        return;
    }

    const std::uint64_t open_now = impl_.TotalChannels() - (discard_old != nullptr ? 1 : 0);
    if (open_now >= impl_.options.max_channels)
    {
        Reject(ENOSPC);
        return;
    }
    if (!admitted)
    {
        if (const int e = impl_.handler.OnAdmit(MakeAdmit(hello, false, false, false)); e != 0)
        {
            Reject(e);
            return;
        }
        admitted = true;
    }

    ChannelOptions co;
    co.tx_capacity = hello.tx_capacity;
    co.rx_capacity = hello.rx_capacity;
    co.tx_max_payload = hello.tx_max_payload;
    co.rx_max_payload = hello.rx_max_payload;
    co.max_ring_capacity = impl_.options.max_ring_capacity;
    const std::string mem_name = "breeze-shm:" + busid_; // 只是方便在 /proc/<pid>/fd 里认出来
    auto created = ShmChannel::Create(co, mem_name.c_str());
    if (!created)
    {
        FailOrDowngrade(created.error(), hello, may_downgrade); // EINVAL（容量不合法）原样告诉对端；ENOMEM、EMFILE……可以退化
        return;
    }
    chan_.emplace(std::move(*created));
    const std::uint64_t bytes = chan_->layout().total_size;
    const std::uint64_t reclaimed = discard_old != nullptr ? discard_old->memory_bytes_ : 0;
    if (impl_.total_bytes - reclaimed + bytes > impl_.options.max_total_bytes)
    {
        chan_.reset();
        FailOrDowngrade(ENOSPC, hello, may_downgrade); // 共享内存的总量配额满了：可以退化成仅 UDS
        return;
    }

    // 先登记 Tx 的 eventfd 再发 Accept：对端拿到 fd 之前谁也不会敲它，登记失败还能干净地拒绝。
    if (const int e = impl_.loop->Add(&tx_wake_, chan_->tx_eventfd(), EPOLLIN); e != 0)
    {
        chan_.reset();
        FailOrDowngrade(e, hello, may_downgrade);
        return;
    }
    tx_registered_ = true;
    tx_ = chan_->TxConsumer();
    rx_ = chan_->RxProducer();

    if (discard_old != nullptr) // 都过了：现在丢旧的，通道名随之空出来
    {
        impl_.discarded.fetch_add(1, std::memory_order_relaxed);
        discard_old->BeginClose(ShmCloseReason::Discarded);
    }

    agent_epoch_ = chan_->BumpAgentEpoch();
    app_epoch_ = chan_->BumpAppEpoch();
    const wire::Reply reply = BuildReply(false, app_epoch_);
    const std::array<int, 3> fds{chan_->memfd(), chan_->tx_eventfd(), chan_->rx_eventfd()};
    if (const int e = sock::SendWithFds(sock_.get(), std::as_bytes(std::span(&reply, 1)), fds); e != 0)
    {
        LOG_DEBUG("shm: 发送 Accept 失败 busid={} errno={}", busid_, e);
        BeginClose(ShmCloseReason::IoError); // 通道还没对外可见（没回调过 OnOpened），按"从未建立"处理
        return;
    }
    memory_bytes_ = bytes;
    Establish(false);
}

// Agent 重启之后，App 带着旧 fd 来恢复。App 交来的 fd 和里面的内容都不可信（设计 §6.5 / §7）。
void ShmServerSession::HandleReattach(const shm::wire::Hello& hello)
{
    using namespace shm;
    if (hello_nfds_ != 3)
    {
        Reject(EINVAL);
        return;
    }
    if (ShmServerSession* old = impl_.FindByBusId(busid_); old != nullptr)
    {
        // 这个通道名在 Agent 里还留着：Agent 没有重启，是 App 这边的 UDS 断了。
        // 交来的必须是同一块内存，否则就是别人占着这个名字。
        old->ProbeAlive();
        if (!old->resumable() || !SameMemory(hello_fds_[0].get(), old->chan_->memfd()))
        {
            Reject(EBUSY);
            return;
        }
        if (old->peer_.uid != peer_.uid)
        {
            Reject(EACCES);
            return;
        }
        if (const int e = impl_.handler.OnAdmit(MakeAdmit(hello, true, true, false)); e != 0)
        {
            Reject(e);
            return;
        }
        ResumeOld(*old, false, false); // 还是原来的 App 进程：app_epoch 不变；它手里有 fd，不用再发
        return;
    }
    if (impl_.TotalChannels() >= impl_.options.max_channels)
    {
        Reject(ENOSPC);
        return;
    }
    if (const int e = impl_.handler.OnAdmit(MakeAdmit(hello, true, false, false)); e != 0)
    {
        Reject(e);
        return;
    }

    // Attach 校验封印、大小、头、布局、容量范围与两条环的游标（对端此刻还活着，校验对此是安全的）；总是接管 fd。
    AttachLimits limits;
    limits.max_ring_capacity = impl_.options.max_ring_capacity;
    auto attached = ShmChannel::Attach(
        Fd(hello_fds_[0].release()), Fd(hello_fds_[1].release()), Fd(hello_fds_[2].release()), limits);
    if (!attached)
    {
        Reject(attached.error());
        return;
    }
    chan_.emplace(std::move(*attached));
    const std::uint64_t bytes = chan_->layout().total_size;
    if (impl_.total_bytes + bytes > impl_.options.max_total_bytes)
    {
        chan_.reset();
        Reject(ENOSPC);
        return;
    }
    tx_ = chan_->TxConsumer();
    rx_ = chan_->RxProducer();
    if (tx_.corrupt() || rx_.corrupt())
    {
        chan_.reset();
        Reject(EINVAL);
        return;
    }
    // 上一任 Agent 可能死在睡着之后：标志还停在 1，先清掉（之后第一轮消费循环无条件扫一遍，收尾时再按协议置位）。
    chan_->tx_control().consumer_sleeping.store(0, std::memory_order_relaxed);
    // 上一任也可能死在处理一条记录的期间：跳过那一条（至多一次），不重发，也不会被毒包拖进崩溃循环。
    const RecoverStatus rs = tx_.RecoverFromCrash();
    if (rs == RecoverStatus::kCorrupt)
    {
        chan_.reset();
        Reject(EINVAL);
        return;
    }
    if (const int e = impl_.loop->Add(&tx_wake_, chan_->tx_eventfd(), EPOLLIN); e != 0)
    {
        chan_.reset();
        Reject(e);
        return;
    }
    tx_registered_ = true;

    agent_epoch_ = chan_->BumpAgentEpoch();
    app_epoch_ = chan_->header().app_epoch.load(std::memory_order_acquire); // 还是原来的 App：不加
    const wire::Reply reply = BuildReply(true, app_epoch_);
    if (const int e = sock::SendWithFds(sock_.get(), std::as_bytes(std::span(&reply, 1)), {}); e != 0)
    {
        LOG_DEBUG("shm: 发送 Accept 失败 busid={} errno={}", busid_, e);
        BeginClose(ShmCloseReason::IoError);
        return;
    }
    if (rs == RecoverStatus::kSkipped)
        impl_.inflight_skipped.fetch_add(1, std::memory_order_relaxed);
    impl_.reattached.fetch_add(1, std::memory_order_relaxed);
    memory_bytes_ = bytes;
    Establish(true);
}

void ShmServerSession::Establish(bool reattached)
{
    ReleasePendingSlot();
    if (timer_ != 0)
    {
        impl_.loop->Cancel(timer_);
        timer_ = 0;
    }
    state_ = State::Open;
    opened_ = true;
    reattached_ = reattached;
    impl_.busids.emplace(busid_, id_);
    impl_.total_bytes += memory_bytes_;
    impl_.open_channels.fetch_add(1, std::memory_order_relaxed);
    impl_.opened.fetch_add(1, std::memory_order_relaxed);
    LOG_DEBUG("shm: 通道建立 busid={} pid={} uid={} 内存={}B reattach={}",
              busid_,
              peer_.pid,
              peer_.uid,
              memory_bytes_,
              reattached);
    impl_.handler.OnOpened(*this);
    // 必须先跑一次消费循环：它收尾时的 PrepareToSleep 才会把 consumer_sleeping 置 1，
    // 之后 App 的第一次 Commit 才知道要敲 eventfd；否则标志一直是 0，服务端永远等不到第一声。
    RunReadable();
}

// ---- ShmServerSession：握手之后 ----

// 握手之后：UDS 上只有 App → Agent 的定长控制消息（Control）和 EOF。别的一律是协议错误。
void ShmServerSession::DrainOpenSocket(std::uint32_t events)
{
    for (;;)
    {
        const ssize_t n =
            ::recv(sock_.get(), ctl_buf_.data() + ctl_len_, ctl_buf_.size() - ctl_len_, MSG_DONTWAIT);
        if (n > 0)
        {
            ctl_len_ += static_cast<std::size_t>(n);
            if (!ControlPrefixOk(std::span<const std::byte>(ctl_buf_.data(), ctl_len_)))
            {
                impl_.protocol_errors.fetch_add(1, std::memory_order_relaxed);
                LOG_WARN("shm: 握手之后对端往 UDS 里写了不是控制消息的数据 busid={}，关闭", busid_);
                BeginClose(ShmCloseReason::ProtocolError);
                return;
            }
            if (ctl_len_ == ctl_buf_.size())
            {
                shm::wire::Control c;
                std::memcpy(&c, ctl_buf_.data(), sizeof(c)); // 先拷到本地再校验
                ctl_len_ = 0;
                if (!HandleControl(c))
                    return;
            }
            continue;
        }
        if (n == 0)
        {
            OnPeerGone(); // EOF：App 没了（含被 kill -9）
            return;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            if ((events & EPOLLERR) != 0)
                BeginClose(ShmCloseReason::IoError);
            else if ((events & (EPOLLHUP | EPOLLRDHUP)) != 0)
                OnPeerGone(); // 挂断了但读不出 EOF：不会发生，保险起见按对端关闭处理
            return;
        }
        if (errno == ECONNRESET)
        {
            // App 没读完我们发的 Accept 就退出（被 kill）时，内核会这样报：也是对端没了，不是 IO 故障。
            OnPeerGone();
            return;
        }
        BeginClose(ShmCloseReason::IoError);
        return;
    }
}

// 返回 false 表示已经因协议错误关闭。
bool ShmServerSession::HandleControl(const shm::wire::Control& c)
{
    using namespace shm;
    if (const int e = wire::CheckControl(c); e != 0)
    {
        impl_.protocol_errors.fetch_add(1, std::memory_order_relaxed);
        LOG_WARN("shm: 控制消息不合法 busid={} errno={}，关闭", busid_, e);
        BeginClose(ShmCloseReason::ProtocolError);
        return false;
    }
    switch (static_cast<wire::ControlOp>(c.op))
    {
        case wire::ControlOp::kPrepareRestart:
            prepared_retain_ = std::min(std::chrono::milliseconds(c.arg), impl_.options.retain_max);
            break;
        case wire::ControlOp::kBye:
            bye_ = true;
            impl_.byes.fetch_add(1, std::memory_order_relaxed);
            break;
    }
    return true;
}

void ShmServerSession::OnTxEvents()
{
    if (!live())
        return;
    tx_.WokeUp(); // 清标志、读清 eventfd
    RunReadable();
}

// 消费循环：回调搬空 → 睡前检查 → 回到 epoll。回调用掉预算时让出，下一轮再来。
// Detached 期间同样要跑：App 崩溃前已经提交的记录是遗言，要处理完。
void ShmServerSession::RunReadable()
{
    constexpr int kMaxImmediateRounds = 8; // 睡前检查发现又有数据而连续重来的次数上限，防止一个通道独占 loop
    for (int round = 0; round < kMaxImmediateRounds; ++round)
    {
        if (!live())
            return;
        const bool more = impl_.handler.OnReadable(*this);
        if (!live())
            return;
        if (more)
            break; // 预算用完：让出
        if (tx_.PrepareToSleep())
        {
            // 真的空了：回 epoll 等 eventfd。不保留的通道遗言已经排空，现在可以销毁了。
            if (state_ == State::Detached && drain_then_close_)
                BeginClose(drain_reason_);
            return;
        }
        // 睡前回头一看又来了记录：再搬一轮
    }
    if (!readable_scheduled_)
    {
        readable_scheduled_ = true;
        impl_.PostReadable(id_);
    }
}

// ---- ShmChannelServer ----

ShmChannelServer::ShmChannelServer(EventLoopGroup& group,
                                   const Endpoint& endpoint,
                                   ShmServerOptions options,
                                   ShmChannelHandler& handler,
                                   ShmUdsDataPath uds)
    : endpoint_(endpoint)
    , impl_(std::make_unique<Impl>(group, endpoint, std::move(options), handler, uds))
{
}

ShmChannelServer::~ShmChannelServer()
{
    Impl& m = *impl_;
    if (m.loop != nullptr && m.group.running())
    {
        // 同步停：不然 Stop 投递的任务可能在本对象销毁之后才在 loop 上跑。
        (void)m.loop->CallSync(
            [&m]
            {
                m.StopOnLoop();
                return 0;
            });
        // 栅栏：事件分发阶段里 PostTeardown / PostReadable 投递的任务可能排在上面这条同步任务之后，
        // 它们里面带着 Impl 的指针，要在 Impl 销毁之前跑完（满载下 asan 抓到过：Teardown 在 Impl 释放之后执行）。
        // 此刻通道都已销毁，不会再有新的投递；队列先进先出，这条任务返回时前面的都跑完了。
        (void)m.loop->CallSync([] { return 0; });
        // 仅 UDS 的连接归连接表持有，OnClosed 要等回收阶段才回调：等它们都回收完再返回，
        // 这样析构返回之后不会再有这个服务端的任何回调（业务的 handler 可以随后销毁），与共享内存的会话一样。
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (m.uds->open.load(std::memory_order_acquire) > 0 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // group 已 Shutdown 的话钩子已经停掉了 Acceptor 并拆除了全部会话，这里的析构不再碰 loop。
}

int ShmChannelServer::Start()
{
    Impl& m = *impl_;
    if (m.endpoint.kind() != Endpoint::Kind::Unix && m.endpoint.kind() != Endpoint::Kind::UnixAbstract)
        return EPROTONOSUPPORT; // fd 只能经 UDS 传递
    if (!m.group.running())
    {
        if (m.group.stopping())
            return ESHUTDOWN;
        LOG_FATAL("ShmChannelServer::Start 之前必须已 EventLoopGroup::Start() 成功 endpoint={}", m.endpoint.ToString());
    }
    if (m.acceptor)
        LOG_FATAL("ShmChannelServer 重复 Start endpoint={}", m.endpoint.ToString());

    m.loop = &GroupAccess::PickLoop(m.group);
    m.acceptor = std::make_unique<Acceptor>(*m.loop,
                                            m.endpoint,
                                            [&m](UniqueFd fd, std::optional<Endpoint> peer)
                                            { m.OnAccept(std::move(fd), std::move(peer)); });
    if (const int rc = m.loop->CallSync([&m] { return m.StartOnLoop(); }); rc != 0)
    {
        m.acceptor.reset();
        m.loop = nullptr;
        return rc;
    }
    endpoint_ = m.acceptor->endpoint();
    return 0;
}

void ShmChannelServer::Stop()
{
    Impl& m = *impl_;
    if (m.loop == nullptr)
        return;
    m.loop->Post([&m] { m.StopOnLoop(); });
}

ShmServerStats ShmChannelServer::stats() const noexcept
{
    const Impl& m = *impl_;
    ShmServerStats s;
    s.uds_channels = m.uds->open.load(std::memory_order_relaxed);
    s.uds_total = m.uds->total.load(std::memory_order_relaxed);
    s.downgraded = m.uds->downgraded.load(std::memory_order_relaxed);
    s.opened = m.opened.load(std::memory_order_relaxed) + s.uds_total;
    s.closed = m.closed.load(std::memory_order_relaxed) + m.uds->closed.load(std::memory_order_relaxed);
    s.open_channels = m.open_channels.load(std::memory_order_relaxed) + s.uds_channels;
    s.rejected = m.rejected.load(std::memory_order_relaxed);
    s.handshake_timeouts = m.handshake_timeouts.load(std::memory_order_relaxed);
    s.pending_overflow = m.pending_overflow.load(std::memory_order_relaxed);
    s.protocol_errors = m.protocol_errors.load(std::memory_order_relaxed);
    s.detached_channels = m.detached_channels.load(std::memory_order_relaxed);
    s.detached_total = m.detached_total.load(std::memory_order_relaxed);
    s.resumed = m.resumed.load(std::memory_order_relaxed);
    s.reattached = m.reattached.load(std::memory_order_relaxed);
    s.retention_expired = m.retention_expired.load(std::memory_order_relaxed);
    s.discarded = m.discarded.load(std::memory_order_relaxed);
    s.byes = m.byes.load(std::memory_order_relaxed);
    s.inflight_skipped = m.inflight_skipped.load(std::memory_order_relaxed);
    s.rx_bytes_discarded = m.rx_bytes_discarded.load(std::memory_order_relaxed);
    return s;
}

} // namespace breeze
