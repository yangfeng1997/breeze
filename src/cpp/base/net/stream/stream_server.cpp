#include "base/net/stream/stream_server.h"

#include <cerrno>
#include <chrono>
#include <utility>

#include "base/logger/logger.h"
#include "base/logger/logger_macros.h"
#include "base/net/loop/group_access.h"
#include "base/net/stream/internal/acceptor.h"
#include "base/net/stream/internal/stream_connection.h"
#include "base/net/stream/internal/stream_socket_config.h"

namespace breeze
{

struct StreamServer::Impl
{
    Impl(EventLoopGroup& g, Endpoint ep, StreamServerOptions o, IConnectionHandler& h, const IFrameCodecFactory& f)
        : group(g)
        , endpoint(std::move(ep))
        , options(std::move(o))
        , handler(h)
        , factory(f)
    {
    }

    EventLoopGroup& group;
    Endpoint endpoint;
    StreamServerOptions options;
    IConnectionHandler& handler;
    const IFrameCodecFactory& factory;

    EventLoop* loop = nullptr; // Acceptor 所在的 loop，Start 时选定
    std::unique_ptr<Acceptor> acceptor;
    EventLoop::HookId hook = 0;

    // 过载时每秒可能有成千上万条连接被拒，日志限频（只在 accept 所在线程访问）。
    EventLoop::Clock::time_point last_reject_log{};
    std::uint64_t suppressed_rejects = 0;

    int StartOnLoop();
    void StopOnLoop();
    void OnAccept(UniqueFd fd, std::optional<Endpoint> peer);
    void Adopt(EventLoop& target, UniqueFd fd, PeerInfo info);
    bool ShouldLogReject();
};

int StreamServer::Impl::StartOnLoop()
{
    if (const int e = acceptor->Listen(options.backlog); e != 0)
        return e;
    // 停机时 loop 自己停掉 accept：group 不认识 StreamServer，只认钩子。
    hook = loop->AddShutdownHook([this] { StopOnLoop(); });
    return 0;
}

void StreamServer::Impl::StopOnLoop()
{
    if (acceptor)
        acceptor->Stop();
    if (hook != 0)
    {
        loop->RemoveShutdownHook(hook);
        hook = 0;
    }
}

bool StreamServer::Impl::ShouldLogReject()
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

// 在 accept 所在 loop 的线程上：过滤 → 选 loop 并预留名额 → 交给目标 loop。
void StreamServer::Impl::OnAccept(UniqueFd fd, std::optional<Endpoint> peer)
{
    PeerInfo info;
    info.endpoint = std::move(peer);
    ConfigureSocket(fd.get(), endpoint.kind(), options.tcp, info); // 差异只在建立时；UDS 在这里取到 credentials

    LoopStats& st = loop->stats();
    if (options.accept_filter && !options.accept_filter(info))
    {
        LoopStats::Bump(st.rejected_filter); // fd 随之关闭，没有任何回调
        return;
    }

    // 目标 loop：连接数最少的。名额在这里（accept 线程）先占，连接真正建起来（在目标线程上）之前也算在内，
    // 所以一阵突发 accept 不会因为投递还没落地就全挑同一个 loop，也不会超过容量。
    EventLoop& target = GroupAccess::PickLoop(group);
    if (!target.table().TryReserve())
    {
        // PickLoop 选的是最少的，它都满了就是所有 loop 都满。
        LoopStats::Bump(st.rejected_full);
        if (ShouldLogReject())
            LOG_WARN("连接表已满，拒绝新连接 endpoint={}（此前被限频略去 {} 条）",
                     acceptor->endpoint().ToString(), // 实际绑定的地址：配置里端口可能是 0
                     std::exchange(suppressed_rejects, 0));
        return;
    }

    if (&target == loop)
    {
        Adopt(target, std::move(fd), std::move(info));
        return;
    }
    target.Post([this, &target, fd = std::move(fd), info = std::move(info)]() mutable
                { Adopt(target, std::move(fd), std::move(info)); });
}

// 在目标 loop 的线程上建连接。codec 也在这里创建：IFrameCodecFactory::Create 本来就要求线程安全。
void StreamServer::Impl::Adopt(EventLoop& target, UniqueFd fd, PeerInfo info)
{
    if (!target.accepting())
    {
        target.table().Unreserve(); // 正在停机：丢掉，不产生回调
        return;
    }
    StreamConnection* c =
        StreamConnection::Create(target, std::move(fd), std::move(info), handler, factory.Create(), options.connection);
    if (c->Establish())
        LoopStats::Bump(target.stats().accepted);
    else
        LoopStats::Bump(target.stats().rejected_resource); // epoll 登记失败：Establish 已记日志，只关了这一条
}

StreamServer::StreamServer(EventLoopGroup& group,
                           const Endpoint& endpoint,
                           StreamServerOptions options,
                           IConnectionHandler& handler,
                           const IFrameCodecFactory& factory)
    : endpoint_(endpoint)
    , impl_(std::make_unique<Impl>(group, endpoint, std::move(options), handler, factory))
{
}

StreamServer::~StreamServer()
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
    }
    // group 已 Shutdown 的话 Acceptor 已被停机钩子停掉，这里的析构不再碰 loop。
}

int StreamServer::Start()
{
    Impl& m = *impl_;
    if (m.endpoint.kind() == Endpoint::Kind::Udp)
        return EPROTONOSUPPORT; // 数据报端点要数据报的服务端（rudp），字节流的不收
    if (!m.group.running())
    {
        if (m.group.stopping())
            return ESHUTDOWN; // group 正在 / 已经停机
        LOG_FATAL("StreamServer::Start 之前必须已 EventLoopGroup::Start() 成功 endpoint={}", m.endpoint.ToString());
    }
    if (m.acceptor)
        LOG_FATAL("StreamServer 重复 Start endpoint={}", m.endpoint.ToString());

    m.loop = &GroupAccess::PickLoop(m.group);
    m.acceptor = std::make_unique<Acceptor>(*m.loop,
                                            m.endpoint,
                                            [&m](UniqueFd fd, std::optional<Endpoint> peer)
                                            { m.OnAccept(std::move(fd), std::move(peer)); });
    // Listen 只能在 loop 线程上做；loop 已在运行，投过去等结果。
    if (const int rc = m.loop->CallSync([&m] { return m.StartOnLoop(); }); rc != 0)
    {
        m.acceptor.reset(); // Listen 失败时没有残留 fd，Acceptor 析构是空操作
        m.loop = nullptr;
        return rc;
    }
    endpoint_ = m.acceptor->endpoint();
    return 0;
}

void StreamServer::Stop()
{
    Impl& m = *impl_;
    if (m.loop == nullptr)
        return;
    m.loop->Post([&m] { m.StopOnLoop(); });
}

} // namespace breeze
