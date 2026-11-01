#include "base/net/agent/agent_client.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#include "base/logger/logger.h"
#include "base/logger/logger_macros.h"
#include "base/net/agent/shm_connection.h"
#include "base/net/codec/length_prefix_codec.h"
#include "base/net/loop/event_loop.h"
#include "base/net/loop/group_access.h"
#include "base/net/socket/unique_fd.h"
#include "base/net/stream/internal/backoff.h"
#include "base/net/stream/internal/fd_passing.h"
#include "base/net/stream/internal/stream_connection.h"
#include "base/net/stream/internal/stream_socket.h"

namespace breeze::agent
{

namespace
{

namespace wire = shm::wire;

// 这些错误重试也没用（配置不对、协议不对、身份被拒、被别的进程接管）：不再重试，要调用方处理。
bool NonRetryable(int e) noexcept
{
    return e == EINVAL || e == EPROTO || e == ESTALE || e == EACCES || e == ENOTSUP || e == EPROTONOSUPPORT;
}

// discard_extra：这次 Attach 不管配置怎么写都要丢弃 Agent 保留着的队列（保留着的那块内存已经不可信）。
wire::Hello MakeHello(const AgentClientOptions& o, wire::AttachType type, bool discard_extra = false)
{
    wire::Hello h{};
    h.hdr = wire::MakeHeader(wire::MsgType::kHello);
    h.attach_type = static_cast<std::uint32_t>(type);
    // Reattach 恢复的是手里已有的共享内存：只能是共享内存，不能退化。
    h.transport = static_cast<std::uint32_t>(type == wire::AttachType::kReattach ? wire::Transport::kShmRequired
                                                                                 : o.transport);
    h.pid = static_cast<std::int32_t>(::getpid());
    if (type == wire::AttachType::kAttach)
    {
        h.flags = (o.discard_retained || discard_extra) ? wire::kFlagDiscardRetained : 0;
        h.tx_capacity = o.tx_capacity;
        h.rx_capacity = o.rx_capacity;
        h.tx_max_payload = o.tx_max_payload;
        h.rx_max_payload = o.rx_max_payload;
    }
    (void)wire::SetBusId(h, o.busid); // Start 已经校验过
    return h;
}

// Reply 里的布局与本地从共享内存头算出来的互相核对：对不上说明 Agent 或共享内存不对劲。
bool LayoutMatches(const wire::Reply& r, const shm::ChannelLayout& l)
{
    return r.total_size == l.total_size && r.tx.capacity == l.tx.capacity && r.tx.max_payload == l.tx.max_payload &&
           r.rx.capacity == l.rx.capacity && r.rx.max_payload == l.rx.max_payload && r.tx.reserved == 0 &&
           r.rx.reserved == 0 && r.reserved == 0;
}

// ---- 一次非阻塞的握手：连接 → 发 Hello（Reattach 带 fd）→ 收 Reply（可能带 fd）----

class Handshaker final : private IPollable
{
public:
    struct Result
    {
        int err = 0;
        UniqueFd sock;
        wire::Reply reply{};
        std::array<UniqueFd, 4> fds; // 比期望的 3 个多留一个槽位：对端多塞会被当成协议错误
        std::size_t nfds = 0;
    };
    using Done = std::move_only_function<void(Result)>;

    Handshaker(EventLoop& loop,
               const Endpoint& ep,
               const wire::Hello& hello,
               std::vector<int> send_fds,
               std::chrono::milliseconds timeout,
               Done done)
        : loop_(loop)
        , ep_(ep)
        , hello_(hello)
        , send_fds_(std::move(send_fds))
        , timeout_(timeout)
        , done_(std::move(done))
    {
    }
    // 不碰 loop：对象可能在 group 已经停掉之后才销毁；要摘 epoll / 取消定时器必须显式 Cancel。
    ~Handshaker() override = default;

    // 开始。返回 errno（同步失败，没有回调）；0 = 进行中，结果一定经 done 回调（除非先 Cancel）。
    int Start()
    {
        loop_.AssertInLoopThread();
        if (const int e = sock::CreateStreamSocket(ep_, sock_); e != 0)
            return e;
        const int c = sock::Connect(sock_.get(), ep_);
        if (c != 0 && c != EINPROGRESS)
            return c;
        state_ = c == 0 ? State::Sending : State::Connecting;
        if (const int e = loop_.Add(this, sock_.get(), EPOLLIN | EPOLLOUT | EPOLLRDHUP); e != 0)
            return e;
        registered_ = true;
        timer_ = loop_.RunAfter(timeout_, [this] { Fail(ETIMEDOUT); });
        return 0;
    }

    // 摘 epoll、取消定时器、关 socket；之后不会再回调 done。
    void Cancel()
    {
        if (timer_ != 0)
        {
            loop_.Cancel(timer_);
            timer_ = 0;
        }
        if (registered_)
        {
            loop_.Del(sock_.get());
            registered_ = false;
        }
        sock_.reset();
        done_ = nullptr;
    }

private:
    enum class State : std::uint8_t
    {
        Connecting,
        Sending,
        Receiving,
        Finished,
    };

    void OnEvents(std::uint32_t events) override
    {
        if (state_ == State::Finished)
            return;
        if (state_ == State::Connecting)
        {
            if ((events & (EPOLLOUT | EPOLLERR | EPOLLHUP)) == 0)
                return;
            if (const int e = sock::GetSocketError(sock_.get()); e != 0)
            {
                Fail(e);
                return;
            }
            state_ = State::Sending;
        }
        if (state_ == State::Sending)
        {
            TrySend();
            if (state_ != State::Receiving)
                return;
        }
        if ((events & (EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0)
            TryRecv();
    }

    void TrySend()
    {
        // Hello 只有 112 字节，一般一次发完；缓冲满（EAGAIN）时整条都没发出去、fd 也没发，等下次可写再来。
        const int e = sock::SendWithFds(sock_.get(), std::as_bytes(std::span(&hello_, 1)), send_fds_);
        if (e == EAGAIN || e == EWOULDBLOCK)
            return;
        if (e != 0)
        {
            Fail(e);
            return;
        }
        state_ = State::Receiving;
        loop_.Mod(this, sock_.get(), EPOLLIN | EPOLLRDHUP); // 不再关心可写
    }

    void TryRecv()
    {
        while (got_ < buf_.size())
        {
            std::array<UniqueFd, 4> chunk;
            std::size_t n = 0, nf = 0;
            const int e = sock::RecvWithFds(sock_.get(), std::span(buf_).subspan(got_), n, chunk, nf);
            if (e == EAGAIN || e == EWOULDBLOCK)
                return;
            if (e != 0)
            {
                Fail(e == EMSGSIZE ? EPROTO : e); // 附带的 fd 太多 = 协议错误
                return;
            }
            if (n == 0)
            {
                Fail(ECONNRESET); // 没回复就关了
                return;
            }
            got_ += n;
            for (std::size_t i = 0; i < nf; ++i)
            {
                if (nfds_ >= fds_.size())
                {
                    Fail(EPROTO);
                    return;
                }
                fds_[nfds_++] = std::move(chunk[i]);
            }
        }
        wire::Reply reply;
        std::memcpy(&reply, buf_.data(), sizeof(reply)); // 先拷到本地再校验
        if (const int e = wire::CheckReplyHeader(reply); e != 0)
        {
            Fail(e);
            return;
        }
        if (reply.result != 0)
        {
            Fail(reply.result); // Reject：原因就是 errno（附带的 fd 如果有，随本对象析构关闭）
            return;
        }
        Finish(reply);
    }

    void Finish(const wire::Reply& reply)
    {
        state_ = State::Finished;
        Result r;
        r.reply = reply;
        r.nfds = nfds_;
        r.fds = std::move(fds_);
        Detach();
        r.sock = std::move(sock_);
        Done done = std::move(done_);
        if (done)
            done(std::move(r)); // 之后不再碰本对象的成员：调用方可能在回调里销毁它
    }

    void Fail(int err)
    {
        state_ = State::Finished;
        Detach();
        sock_.reset();
        Done done = std::move(done_);
        if (done)
        {
            Result r;
            r.err = err;
            done(std::move(r));
        }
    }

    void Detach()
    {
        if (timer_ != 0)
        {
            loop_.Cancel(timer_);
            timer_ = 0;
        }
        if (registered_)
        {
            loop_.Del(sock_.get());
            registered_ = false;
        }
    }

    EventLoop& loop_;
    Endpoint ep_;
    wire::Hello hello_;
    std::vector<int> send_fds_;
    std::chrono::milliseconds timeout_;
    Done done_;
    UniqueFd sock_;
    State state_ = State::Connecting;
    bool registered_ = false;
    EventLoop::TimerId timer_ = 0;
    std::array<std::byte, sizeof(wire::Reply)> buf_{};
    std::size_t got_ = 0;
    std::array<UniqueFd, 4> fds_;
    std::size_t nfds_ = 0;
};

// ---- 共享内存模式下握手用的那条 UDS：只用来发现 Agent 没了，和发控制消息 ----

class LinkWatcher final : private IPollable
{
public:
    LinkWatcher(EventLoop& loop, UniqueFd sock, std::function<void()> on_gone)
        : loop_(loop)
        , sock_(std::move(sock))
        , on_gone_(std::move(on_gone))
    {
    }
    ~LinkWatcher() override = default; // 不碰 loop，见 Handshaker

    int Start()
    {
        if (const int e = loop_.Add(this, sock_.get(), EPOLLIN | EPOLLRDHUP); e != 0)
            return e;
        registered_ = true;
        return 0;
    }
    void Cancel()
    {
        if (registered_)
        {
            loop_.Del(sock_.get());
            registered_ = false;
        }
        sock_.reset();
        on_gone_ = nullptr;
    }
    // 尽力而为：App 退出时 Agent 反正会读到 EOF。
    void SendControl(wire::ControlOp op, std::uint32_t arg)
    {
        if (!sock_.valid())
            return;
        const wire::Control c = wire::MakeControl(op, arg);
        (void)::send(sock_.get(), &c, sizeof(c), MSG_NOSIGNAL | MSG_DONTWAIT);
    }

private:
    void OnEvents(std::uint32_t events) override
    {
        if (!sock_.valid())
            return;
        std::array<std::byte, 64> sink;
        for (;;)
        {
            const ssize_t n = ::recv(sock_.get(), sink.data(), sink.size(), MSG_DONTWAIT);
            if (n > 0)
            {
                Gone(); // 握手之后 Agent 不该往 UDS 里写任何东西：当作断开
                return;
            }
            if (n == 0)
            {
                Gone(); // EOF：Agent 退出了（含被 kill）
                return;
            }
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                if ((events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0)
                    Gone();
                return;
            }
            Gone(); // ECONNRESET 等
            return;
        }
    }
    void Gone()
    {
        auto cb = std::move(on_gone_);
        Cancel(); // 摘 epoll、关 socket
        if (cb)
            cb(); // 之后不再碰本对象的成员
    }

    EventLoop& loop_;
    UniqueFd sock_;
    std::function<void()> on_gone_;
    bool registered_ = false;
};

// 仅 UDS 的数据通道：StreamConnection 上挂一份中间处理器的引用，让它活得比连接久。
struct KeepAliveContext final : ConnectionContext
{
    std::shared_ptr<void> keepalive;
};

} // namespace

namespace
{

// 中间处理器：先转给业务，再通知 Impl（重连、清状态）。连接归连接表持有，可能比 AgentClient 活得久，所以由 shared_ptr 持有，
// 每条连接都持一份；Impl 的指针在析构时置空（那时连接都已回收完）。
class Adapter final : public IConnectionHandler
{
public:
    explicit Adapter(IConnectionHandler& user)
        : user_(user)
    {
    }
    void OnConnected(IConnection& c) noexcept override { user_.OnConnected(c); }
    void OnMessage(IConnection& c, std::span<const std::byte> payload) noexcept override { user_.OnMessage(c, payload); }
    void OnClosed(IConnection& c, CloseReason r, int e) noexcept override;

    std::atomic<AgentClient::Impl*> impl{nullptr};
    std::atomic<int> live{0}; // 已经建起、还没回收完的连接数

private:
    IConnectionHandler& user_;
};

} // namespace

struct AgentClient::Impl
{
    enum class State : std::uint8_t
    {
        Idle,
        Handshaking, // 首次握手 / 连接关闭之后的重新握手
        Up,          // 已连上
        Reattaching, // 共享内存模式：Agent 掉了，在后台 Reattach
        Backoff,     // 等退避定时器，再握手 / 再 Reattach
    };

    Impl(EventLoopGroup& g, Endpoint e, AgentClientOptions o, IConnectionHandler& u)
        : group(g)
        , ep(std::move(e))
        , opt(std::move(o))
        , adapter(std::make_shared<Adapter>(u))
        , backoff(opt.reconnect, reinterpret_cast<std::uintptr_t>(this) ^ 0x9E3779B97F4A7C15ULL)
        , shm_stats(std::make_shared<ShmConnStats>())
    {
        adapter->impl.store(this);
    }

    EventLoopGroup& group;
    Endpoint ep;
    AgentClientOptions opt;
    std::shared_ptr<Adapter> adapter;
    LengthPrefixCodecFactory default_codec;

    std::mutex pick_mu;
    EventLoop* loop = nullptr;
    std::atomic<std::uint64_t> handle_raw{0};
    std::atomic<AgentClient::Link> link_kind{AgentClient::Link::kNone};
    std::atomic<bool> agent_down{false};
    std::atomic<std::uint64_t> attaches{0}, attach_failures{0}, reattaches{0}, reattach_failures{0}, agent_down_total{0};
    std::atomic<std::uint64_t> last_backoff_ms{0};

    // ---- 只在 loop 线程上访问 ----
    State state = State::Idle;
    std::uint64_t epoch = 0; // 每次 Start / Stop 加一：迟到的回调与定时器靠它识别自己已经过期
    std::unique_ptr<Handshaker> hs;
    EventLoop::TimerId timer = 0;
    Backoff backoff;
    ShmConnection* shm = nullptr;
    IConnection* uds = nullptr;
    std::unique_ptr<LinkWatcher> watcher;
    std::uint64_t app_epoch = 0;
    std::uint64_t agent_epoch = 0;
    // 下一次 Attach 要丢弃 Agent 保留着的队列：共享内存里的环被对端写坏、或接手保留着的内存时校验不过（再恢复它只会再坏一次）。
    bool discard_next = false;
    bool prepared_restart = false; // 调过 PrepareRestart：之后的 Stop / 析构当作"重启"，不发 Bye（发了 Agent 就不保留了）
    std::shared_ptr<ShmConnStats> shm_stats;

    // ---- 流程 ----
    void StartOnLoop();
    void StopOnLoop();
    void BeginAttach();
    void OnAttachDone(std::uint64_t e, Handshaker::Result r);
    void OnAttachFailed(int err);
    void ScheduleRetry();
    void EstablishShm(Handshaker::Result r);
    void EstablishUds(Handshaker::Result r);
    void OnAgentGone(std::uint64_t e);
    void BeginReattach();
    void OnReattachDone(std::uint64_t e, Handshaker::Result r);
    void OnReattachFailed(int err);
    void OnConnectionClosed(IConnection* c, CloseReason reason);
    void DropHandshaker();
    void CancelTimer();
    void ClearLink();
    // 起 Handshaker 并处理同步失败；成功返回 true。
    bool StartHandshaker(const wire::Hello& hello, std::vector<int> fds, Handshaker::Done done, int* err);
};

void Adapter::OnClosed(IConnection& c, CloseReason r, int e) noexcept
{
    user_.OnClosed(c, r, e);
    if (AgentClient::Impl* i = impl.load(std::memory_order_acquire))
        i->OnConnectionClosed(&c, r);
    live.fetch_sub(1, std::memory_order_release); // 最后一步：AgentClient 的析构在等它减到 0
}

// ---- Impl：流程 ----

bool AgentClient::Impl::StartHandshaker(const wire::Hello& hello, std::vector<int> fds, Handshaker::Done done, int* err)
{
    hs = std::make_unique<Handshaker>(*loop, ep, hello, std::move(fds), opt.handshake_timeout, std::move(done));
    if (const int e = hs->Start(); e != 0)
    {
        hs.reset(); // 没登记任何东西，可以直接销毁
        *err = e;
        return false;
    }
    return true;
}

void AgentClient::Impl::CancelTimer()
{
    if (timer != 0)
    {
        loop->Cancel(timer);
        timer = 0;
    }
}

// 握手回调栈里不能销毁 Handshaker 自己：挪到一个任务里，本轮分发之后再销毁。
void AgentClient::Impl::DropHandshaker()
{
    if (hs)
        loop->Post([old = std::move(hs)] {});
}

void AgentClient::Impl::ClearLink()
{
    handle_raw.store(0, std::memory_order_release);
    link_kind.store(AgentClient::Link::kNone, std::memory_order_release);
    agent_down.store(false, std::memory_order_release);
    if (watcher)
    {
        watcher->Cancel();
        watcher.reset();
    }
    shm = nullptr;
    uds = nullptr;
}

void AgentClient::Impl::StartOnLoop()
{
    if (state != State::Idle)
        return; // 已经在跑
    ++epoch;
    backoff.Reset();
    BeginAttach();
}

void AgentClient::Impl::BeginAttach()
{
    state = State::Handshaking;
    const std::uint64_t e = epoch;
    int err = 0;
    if (!StartHandshaker(MakeHello(opt, wire::AttachType::kAttach, discard_next),
                         {},
                         [this, e](Handshaker::Result r) { OnAttachDone(e, std::move(r)); },
                         &err))
        OnAttachFailed(err);
}

void AgentClient::Impl::OnAttachDone(std::uint64_t e, Handshaker::Result r)
{
    DropHandshaker();
    if (e != epoch || state != State::Handshaking)
        return; // 过期（期间 Stop 过）
    if (r.err != 0)
    {
        OnAttachFailed(r.err);
        return;
    }
    discard_next = false; // 带着 discard 的 Hello 被接受了：这一回合用掉了
    if (r.reply.transport == static_cast<std::uint32_t>(wire::ChosenTransport::kShm))
        EstablishShm(std::move(r));
    else if (r.reply.transport == static_cast<std::uint32_t>(wire::ChosenTransport::kUds))
        EstablishUds(std::move(r));
    else
        OnAttachFailed(EPROTO);
}

void AgentClient::Impl::OnAttachFailed(int err)
{
    attach_failures.fetch_add(1, std::memory_order_relaxed);
    const bool retry = !NonRetryable(err);
    LOG_DEBUG("agent client: 握手失败 busid={} errno={} 重试={}", opt.busid, err, retry);
    if (opt.on_connect_failed)
        opt.on_connect_failed(err, retry);
    if (!retry)
    {
        state = State::Idle;
        return;
    }
    ScheduleRetry();
}

void AgentClient::Impl::ScheduleRetry()
{
    state = State::Backoff;
    const std::uint64_t e = epoch;
    CancelTimer();
    const std::chrono::milliseconds delay = backoff.Next();
    last_backoff_ms.store(static_cast<std::uint64_t>(delay.count()), std::memory_order_relaxed);
    timer = loop->RunAfter(delay,
                           [this, e]
                           {
                               timer = 0;
                               if (e != epoch || state != State::Backoff)
                                   return;
                               if (shm != nullptr && agent_down.load(std::memory_order_acquire))
                                   BeginReattach();
                               else
                                   BeginAttach();
                           });
}

void AgentClient::Impl::EstablishShm(Handshaker::Result r)
{
    // 我们要的是仅 UDS 却拿到共享内存，或 fd 数量不对：协议错误。
    if (opt.transport == wire::Transport::kUdsOnly || r.nfds != 3)
    {
        OnAttachFailed(EPROTO);
        return;
    }
    auto chan = shm::ShmChannel::Attach(
        shm::Fd(r.fds[0].release()), shm::Fd(r.fds[1].release()), shm::Fd(r.fds[2].release()), opt.limits);
    if (!chan || !LayoutMatches(r.reply, chan->layout()) || r.reply.resumed > 1)
    {
        const int err = chan ? EPROTO : chan.error();
        if (r.reply.resumed == 1)
        {
            // Agent 把保留着的那块内存还给了我们，但校验不过（环被写坏）：它不会自己变好，下一次 Attach 丢弃它重建，
            // 而不是把 EINVAL 当成不可重试的配置错误停在 Idle。
            LOG_WARN("agent client: 保留着的共享内存校验不过 busid={} errno={}，下一次 Attach 丢弃重建", opt.busid, err);
            discard_next = true;
            attach_failures.fetch_add(1, std::memory_order_relaxed);
            if (opt.on_connect_failed)
                opt.on_connect_failed(err, true);
            ScheduleRetry();
            return;
        }
        OnAttachFailed(err);
        return;
    }
    if (!loop->table().TryReserve()) // 连接表满了
    {
        OnAttachFailed(ENOSPC);
        return;
    }
    auto w = std::make_unique<LinkWatcher>(*loop, std::move(r.sock), [this, e = epoch] { OnAgentGone(e); });
    if (const int err = w->Start(); err != 0)
    {
        loop->table().Unreserve();
        OnAttachFailed(err);
        return;
    }
    PeerInfo info;
    info.endpoint = ep;
    ShmConnection* conn = ShmConnection::Create(
        *loop, std::move(*chan), std::move(info), *adapter, opt.connection, opt.rx_budget, shm_stats, adapter);
    watcher = std::move(w);
    shm = conn;
    app_epoch = r.reply.app_epoch;
    agent_epoch = r.reply.agent_epoch;
    handle_raw.store(conn->handle().raw(), std::memory_order_release);
    link_kind.store(AgentClient::Link::kShm, std::memory_order_release);
    agent_down.store(false, std::memory_order_release);
    adapter->live.fetch_add(1, std::memory_order_relaxed);
    state = State::Up;
    backoff.Reset();
    attaches.fetch_add(1, std::memory_order_relaxed);
    if (!conn->Establish())
    {
        // 没回调过 OnConnected，也就不会有 OnClosed：自己撤销。
        adapter->live.fetch_sub(1, std::memory_order_relaxed);
        ClearLink();
        OnAttachFailed(ENOMEM);
    }
}

void AgentClient::Impl::EstablishUds(Handshaker::Result r)
{
    // 我们要的是共享内存（必须）却被退化，或者带了 fd：协议错误。
    if (opt.transport == wire::Transport::kShmRequired || r.nfds != 0)
    {
        OnAttachFailed(EPROTO);
        return;
    }
    if (!loop->table().TryReserve())
    {
        OnAttachFailed(ENOSPC);
        return;
    }
    const IFrameCodecFactory& factory = opt.codec != nullptr ? *opt.codec : static_cast<const IFrameCodecFactory&>(default_codec);
    PeerInfo info;
    info.endpoint = ep;
    StreamConnection* conn =
        StreamConnection::Create(*loop, std::move(r.sock), std::move(info), *adapter, factory.Create(), opt.connection);
    auto ctx = std::make_unique<KeepAliveContext>();
    ctx->keepalive = adapter;
    conn->SetContext(std::move(ctx));
    uds = conn;
    handle_raw.store(conn->handle().raw(), std::memory_order_release);
    link_kind.store(AgentClient::Link::kUds, std::memory_order_release);
    adapter->live.fetch_add(1, std::memory_order_relaxed);
    state = State::Up;
    backoff.Reset();
    attaches.fetch_add(1, std::memory_order_relaxed);
    if (!conn->Establish())
    {
        adapter->live.fetch_sub(1, std::memory_order_relaxed);
        ClearLink();
        OnAttachFailed(ENOMEM);
    }
}

// 共享内存模式：握手那条 UDS 断了（Agent 退出或被 kill）。连接不关，后台 Reattach。
void AgentClient::Impl::OnAgentGone(std::uint64_t e)
{
    if (e != epoch || state != State::Up || shm == nullptr)
        return;
    watcher.reset(); // 已经 Cancel 过（socket 已关）
    agent_down.store(true, std::memory_order_release);
    agent_down_total.fetch_add(1, std::memory_order_relaxed);
    LOG_WARN("agent client: Agent 掉线 busid={}，后台 Reattach", opt.busid);
    backoff.Reset();
    BeginReattach();
}

void AgentClient::Impl::BeginReattach()
{
    if (shm == nullptr)
        return;
    state = State::Reattaching;
    const std::uint64_t e = epoch;
    shm::ShmChannel& ch = shm->channel();
    int err = 0;
    if (!StartHandshaker(MakeHello(opt, wire::AttachType::kReattach),
                         {ch.memfd(), ch.tx_eventfd(), ch.rx_eventfd()},
                         [this, e](Handshaker::Result r) { OnReattachDone(e, std::move(r)); },
                         &err))
        OnReattachFailed(err);
}

void AgentClient::Impl::OnReattachDone(std::uint64_t e, Handshaker::Result r)
{
    DropHandshaker();
    if (e != epoch || state != State::Reattaching || shm == nullptr)
        return;
    if (r.err != 0)
    {
        OnReattachFailed(r.err);
        return;
    }
    // Reattach 的 Accept 不带 fd（我们手里本来就有）；对得上才算数。
    shm::ShmChannel& ch = shm->channel();
    if (r.nfds != 0 || r.reply.resumed != 1 ||
        r.reply.transport != static_cast<std::uint32_t>(wire::ChosenTransport::kShm) ||
        !LayoutMatches(r.reply, ch.layout()))
    {
        OnReattachFailed(EPROTO);
        return;
    }
    // 通道已被别的进程接管（app_epoch 对不上）：本进程不该再用它。
    if (r.reply.app_epoch != app_epoch || ch.header().app_epoch.load(std::memory_order_acquire) != app_epoch)
    {
        OnReattachFailed(ESTALE);
        return;
    }
    // 新 Agent 接手时 agent_epoch 加一；Agent 没重启（还留着这条通道）时不变；倒退说明不对劲。
    if (r.reply.agent_epoch < agent_epoch)
    {
        OnReattachFailed(EPROTO);
        return;
    }
    auto w = std::make_unique<LinkWatcher>(*loop, std::move(r.sock), [this, e = epoch] { OnAgentGone(e); });
    if (const int err = w->Start(); err != 0)
    {
        OnReattachFailed(err);
        return;
    }
    watcher = std::move(w);
    agent_epoch = r.reply.agent_epoch;
    agent_down.store(false, std::memory_order_release);
    state = State::Up;
    backoff.Reset();
    reattaches.fetch_add(1, std::memory_order_relaxed);
    LOG_INFO("agent client: Reattach 成功 busid={} agent_epoch={}", opt.busid, agent_epoch);
}

void AgentClient::Impl::OnReattachFailed(int err)
{
    reattach_failures.fetch_add(1, std::memory_order_relaxed);
    if (NonRetryable(err))
    {
        // 这条通道没救了（校验不过、被接管）：关掉，OnConnectionClosed 里会走全新的 Attach。
        LOG_WARN("agent client: Reattach 不可能成功 busid={} errno={}，关闭这条连接重新 Attach", opt.busid, err);
        if (shm != nullptr)
            shm->Close();
        return;
    }
    ScheduleRetry(); // Agent 还没起来（ENOENT / ECONNREFUSED）、超时、忙：退避之后再来
}

// 连接被回收（业务或库关闭、环被写坏、停机……）。
void AgentClient::Impl::OnConnectionClosed(IConnection* c, CloseReason reason)
{
    if (c != shm && c != uds)
        return; // 不是当前这条（过期的）
    // 环被对端写坏（读写环时校验不过）：Agent 那边保留着的是同一块坏内存，重新 Attach 要丢弃它，不然永远恢复不了。
    if (c == shm && reason == CloseReason::ProtocolError)
        discard_next = true;
    DropHandshaker();
    CancelTimer();
    ClearLink();
    if (state == State::Idle)
        return; // Stop 引起的：不再重连
    ScheduleRetry(); // 重新 Attach
}

void AgentClient::Impl::StopOnLoop()
{
    ++epoch;
    state = State::Idle;
    if (hs)
    {
        hs->Cancel();
        hs.reset();
    }
    CancelTimer();
    if (watcher && !prepared_restart)
        watcher->SendControl(wire::ControlOp::kBye, 0); // 正常退出：Agent 排空 Tx 就销毁，不保留
    if (shm != nullptr)
        shm->Close();
    if (uds != nullptr)
        uds->Close();
    // 立刻清掉当前连接的指针（也摘掉探活）：旧连接的 OnClosed 要等回收阶段才到，那时 Start 可能已经开了新一轮，
    // 靠 OnConnectionClosed 里"不是当前这条就忽略"识别它。
    ClearLink();
}

// ---- AgentClient ----

AgentClient::AgentClient(EventLoopGroup& group, Endpoint endpoint, AgentClientOptions options, IConnectionHandler& handler)
    : endpoint_(endpoint)
    , impl_(std::make_unique<Impl>(group, std::move(endpoint), std::move(options), handler))
{
}

AgentClient::~AgentClient()
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
        // 栅栏：此前投递的任务（延后销毁 Handshaker、读环、定时器回调……）都要在 Impl 销毁之前跑完。
        (void)m.loop->CallSync([] { return 0; });
        // 连接归连接表持有，OnClosed 要等回收阶段：等它们都回收完再返回，之后不会再有这个客户端的回调。
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (m.adapter->live.load(std::memory_order_acquire) > 0 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    m.adapter->impl.store(nullptr, std::memory_order_release);
}

int AgentClient::Start()
{
    Impl& m = *impl_;
    if (!wire::IsValidBusId(m.opt.busid))
        return EINVAL;
    if (m.opt.transport < wire::Transport::kUdsOnly || m.opt.transport > wire::Transport::kShmRequired)
        return EINVAL;
    if (!m.opt.reconnect.Valid())
        return EINVAL;
    if (endpoint_.kind() != Endpoint::Kind::Unix && endpoint_.kind() != Endpoint::Kind::UnixAbstract)
        return EPROTONOSUPPORT; // fd 只能经 UDS 传递；仅 UDS 也要先在 UDS 上协商
    if (!m.group.running())
    {
        if (m.group.stopping())
            return ESHUTDOWN;
        LOG_FATAL("AgentClient::Start 之前必须已 EventLoopGroup::Start() 成功 endpoint={}", endpoint_.ToString());
    }
    {
        const std::lock_guard lk(m.pick_mu);
        if (m.loop == nullptr)
            m.loop = &GroupAccess::PickLoop(m.group);
    }
    m.loop->Post([&m] { m.StartOnLoop(); });
    return 0;
}

void AgentClient::Stop()
{
    Impl& m = *impl_;
    EventLoop* loop = nullptr;
    {
        const std::lock_guard lk(m.pick_mu);
        loop = m.loop;
    }
    if (loop != nullptr)
        loop->Post([&m] { m.StopOnLoop(); });
}

void AgentClient::PrepareRestart(std::chrono::milliseconds retain)
{
    Impl& m = *impl_;
    EventLoop* loop = nullptr;
    {
        const std::lock_guard lk(m.pick_mu);
        loop = m.loop;
    }
    if (loop == nullptr || retain.count() <= 0 || retain.count() > static_cast<std::chrono::milliseconds::rep>(UINT32_MAX))
        return;
    loop->Post(
        [&m, retain]
        {
            if (m.watcher)
            {
                m.watcher->SendControl(wire::ControlOp::kPrepareRestart, static_cast<std::uint32_t>(retain.count()));
                m.prepared_restart = true;
            }
        });
}

SendResult AgentClient::Send(SharedBuffer payload)
{
    const ConnectionHandle h(impl_->handle_raw.load(std::memory_order_acquire));
    if (!h.valid())
        return SendResult::Closed;
    return h.Send(std::move(payload));
}

ConnectionHandle AgentClient::handle() const noexcept
{
    return ConnectionHandle(impl_->handle_raw.load(std::memory_order_acquire));
}

AgentClient::Link AgentClient::link() const noexcept { return impl_->link_kind.load(std::memory_order_acquire); }

bool AgentClient::agent_down() const noexcept { return impl_->agent_down.load(std::memory_order_acquire); }

AgentClientStats AgentClient::stats() const noexcept
{
    const Impl& m = *impl_;
    AgentClientStats s;
    s.attaches = m.attaches.load(std::memory_order_relaxed);
    s.attach_failures = m.attach_failures.load(std::memory_order_relaxed);
    s.reattaches = m.reattaches.load(std::memory_order_relaxed);
    s.reattach_failures = m.reattach_failures.load(std::memory_order_relaxed);
    s.agent_down_total = m.agent_down_total.load(std::memory_order_relaxed);
    s.tx_messages = m.shm_stats->tx_messages.load(std::memory_order_relaxed);
    s.tx_full_drops = m.shm_stats->tx_full_drops.load(std::memory_order_relaxed);
    s.tx_too_large = m.shm_stats->tx_too_large.load(std::memory_order_relaxed);
    s.rx_messages = m.shm_stats->rx_messages.load(std::memory_order_relaxed);
    s.last_backoff_ms = m.last_backoff_ms.load(std::memory_order_relaxed);
    return s;
}

} // namespace breeze::agent
