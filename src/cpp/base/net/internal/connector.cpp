#include "connector.h"

#include <sys/epoll.h>

#include <cerrno>
#include <chrono>

#include "connection.h"
#include "logger.h"
#include "logger_macros.h"
#include "transport.h"

namespace breeze
{

namespace
{

// 不可重试的 connect 错误用黑名单：其余一律按退避重试——含跨机常见的 ETIMEDOUT / EHOSTUNREACH / ECONNRESET / ENETDOWN，
// 以及 EAGAIN、ECONNREFUSED、ENETUNREACH、EADDRNOTAVAIL、EADDRINUSE、UDS 的 ENOENT（对端尚未创建 socket 文件）。
// 与 muduo Connector 同构（它对 SO_ERROR 里任何非零值也都重试）；白名单会漏掉跨机的 errno。
bool IsFatalConnectError(int err)
{
    switch (err)
    {
        case EACCES:
        case EPERM:
        case EAFNOSUPPORT:
        case EBADF:
        case EFAULT:
        case ENOTSOCK:
        case EINVAL:
        case ENAMETOOLONG:
            return true;
        default:
            return false;
    }
}

std::uint64_t Seed(const void* self)
{
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
           reinterpret_cast<std::uintptr_t>(self);
}

} // namespace

Connector::Connector(EventLoop& loop,
                     Endpoint endpoint,
                     ClientOptions options,
                     const ReconnectPolicy& policy,
                     ConnectionHandler& handler,
                     const FrameCodecFactory& factory,
                     std::atomic<std::uint64_t>& handle)
    : loop_(loop)
    , endpoint_(std::move(endpoint))
    , conn_options_(options.connection)
    , connect_timeout_(options.connect_timeout)
    , on_connect_failed_(std::move(options.on_connect_failed))
    , user_handler_(handler)
    , factory_(factory)
    , handle_(handle)
    , forwarder_(*this, handler)
    , backoff_(policy, Seed(this))
{
}

Connector::~Connector() = default;

void Connector::Start()
{
    loop_.AssertInLoopThread();
    if (running_)
        LOG_FATAL("NetClient::Start 在 Running 状态下重复调用 endpoint={}", endpoint_.ToString());
    running_ = true;
    backoff_.Reset();
    if (awaiting_closed_.load(std::memory_order_relaxed))
    {
        start_when_closed_ = true; // 旧连接的 OnClosed 先于新连接的 OnConnected
        return;
    }
    Attempt();
}

void Connector::CancelAttempt()
{
    if (connect_timer_ != 0)
    {
        loop_.Cancel(connect_timer_);
        connect_timer_ = 0;
    }
    if (retry_timer_ != 0)
    {
        loop_.Cancel(retry_timer_);
        retry_timer_ = 0;
    }
    if (fd_.valid())
    {
        loop_.Del(fd_.get());
        fd_.reset();
    }
}

void Connector::Detach()
{
    loop_.AssertInLoopThread();
    running_ = false;
    start_when_closed_ = false;
    CancelAttempt();
    if (phase_ != Phase::Connected)
        phase_ = Phase::Idle;
}

void Connector::Stop()
{
    loop_.AssertInLoopThread();
    if (!running_)
        return;
    Detach();
    // 先清句柄：之后 NetClient::Send 得到 Closed。连接在 OnClosed 回调时才真正销毁，期间不能开始新的尝试。
    if (const std::uint64_t raw = handle_.exchange(0, std::memory_order_acq_rel); raw != 0)
    {
        const ConnectionHandle h(raw);
        if (Connection* c = loop_.table().Current(h.slot(), h.generation()))
            c->Close();
        awaiting_closed_.store(true, std::memory_order_release);
    }
    phase_ = Phase::Idle;
}

void Connector::Attempt()
{
    phase_ = Phase::Connecting;
    Fd fd;
    int err = sock::Create(endpoint_, fd);
    if (err == 0)
    {
        err = sock::Connect(fd.get(), endpoint_);
        if (err == 0) // UDS 与回环常常立刻连上
        {
            OnSocketConnected(std::move(fd));
            return;
        }
        if (err == EINPROGRESS)
        {
            err = loop_.Add(this, fd.get(), EPOLLOUT); // ERR / HUP 内核总会报
            if (err == 0)
            {
                fd_ = std::move(fd);
                connect_timer_ = loop_.RunAfter(connect_timeout_,
                                                [this]
                                                {
                                                    connect_timer_ = 0;
                                                    OnConnectTimeout();
                                                });
                return;
            }
        }
    }
    Failed(err);
}

void Connector::OnEvents(std::uint32_t)
{
    if (phase_ != Phase::Connecting || !fd_.valid())
        return; // Stop 之后同一批事件里残留的就绪通知
    loop_.Cancel(connect_timer_);
    connect_timer_ = 0;
    loop_.Del(fd_.get());
    Fd fd = std::move(fd_);
    // 非阻塞 connect 以 EPOLLOUT 就绪，成败要读 SO_ERROR 判定。
    if (const int err = sock::SoError(fd.get()); err != 0)
    {
        Failed(err);
        return;
    }
    OnSocketConnected(std::move(fd));
}

void Connector::OnConnectTimeout()
{
    if (phase_ != Phase::Connecting)
        return;
    loop_.Del(fd_.get());
    fd_.reset();
    Failed(ETIMEDOUT);
}

void Connector::OnSocketConnected(Fd fd)
{
    // 回环上 TCP 同时打开会"连到自己"（本地与对端地址、端口相同）：对端其实没在监听，按失败处理。
    if (endpoint_.kind() == Endpoint::Kind::Tcp && sock::IsSelfConnect(fd.get()))
    {
        Failed(ECONNREFUSED);
        return;
    }
    if (!loop_.table().TryReserve())
    {
        Failed(ENOMEM); // 本 loop 的连接表满了：退避之后再试
        return;
    }
    PeerInfo info;
    info.endpoint = endpoint_;
    SetupTransport(fd.get(), endpoint_.kind(), conn_options_, info);

    Connection* c = loop_.NewConnection(std::move(fd), std::move(info), forwarder_, factory_.Create(), conn_options_);
    // 句柄先于 OnConnected 发布：OnConnected 里就能经 NetClient::Send 发数据。
    handle_.store(c->handle().raw(), std::memory_order_release);
    phase_ = Phase::Connected;
    backoff_.Reset();
    if (c->Establish())
    {
        LoopStats::Bump(loop_.stats().connected);
        return;
    }
    // epoll 登记失败：连接已被标记关闭，没有任何回调（包括 OnClosed）。
    handle_.store(0, std::memory_order_release);
    Failed(ENOMEM);
}

void Connector::Failed(int sys_errno)
{
    if (IsFatalConnectError(sys_errno))
    {
        // 先回到 Idle 再回调：回调里直接 Start() 是合法的。
        running_ = false;
        phase_ = Phase::Idle;
        backoff_.Reset();
        LOG_ERROR("连接 {} 失败，不可重试 errno={}", endpoint_.ToString(), sys_errno);
        if (on_connect_failed_)
            on_connect_failed_(sys_errno, false);
        return;
    }
    ScheduleRetry();
    LOG_WARN("连接 {} 失败 errno={}，将重试", endpoint_.ToString(), sys_errno);
    if (on_connect_failed_)
        on_connect_failed_(sys_errno, true);
}

void Connector::ScheduleRetry()
{
    phase_ = Phase::Backoff;
    retry_timer_ = loop_.RunAfter(backoff_.Next(),
                                  [this]
                                  {
                                      retry_timer_ = 0;
                                      Attempt();
                                  });
}

void Connector::OnConnectionClosed(Connection& c, CloseReason reason, int sys_errno)
{
    handle_.store(0, std::memory_order_release); // 先清句柄：回调里的 Send 得到 Closed
    user_handler_.OnClosed(c, reason, sys_errno);
    if (!running_)
    {
        // Stop / 停机：不重连，此后本 client 不再有任何回调。
        // 清 awaiting_closed_ 必须是这里最后一步：NetClient 的析构等的就是它，清了之后本对象随时可能被销毁；
        // 也必须在用户的 OnClosed 返回之后，否则析构会和还在执行的用户回调赛跑。
        awaiting_closed_.store(false, std::memory_order_release);
        return;
    }
    awaiting_closed_.store(false, std::memory_order_release); // running_ 为真时析构会先 Stop()，不会与这里并发
    if (start_when_closed_)
    {
        start_when_closed_ = false;
        // Stop 之后马上又 Start：不走退避。但不能在这里直接 Attempt：旧连接的槽要到本轮回收才释放，
        // 连接表恰好满时 TryReserve 会失败、误报 ENOMEM 并退避。定时器下一轮才触发，那时槽已经回收。
        phase_ = Phase::Backoff;
        retry_timer_ = loop_.RunAfter(std::chrono::milliseconds::zero(),
                                      [this]
                                      {
                                          retry_timer_ = 0;
                                          Attempt();
                                      });
        return;
    }
    ScheduleRetry(); // 断开立即已经发了 OnClosed；退避之后重连，连接成功才重置退避
}

} // namespace breeze
