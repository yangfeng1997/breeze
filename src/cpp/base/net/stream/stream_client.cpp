#include "stream_client.h"

#include <cerrno>
#include <chrono>
#include <thread>

#include "connector.h"
#include "group_access.h"
#include "logger.h"
#include "logger_macros.h"

namespace breeze
{

struct StreamClient::Impl
{
    Impl(EventLoopGroup& g, StreamClientOptions o, ReconnectPolicy p, ConnectionHandler& h, const FrameCodecFactory& f)
        : group(g)
        , options(std::move(o))
        , policy(p)
        , handler(h)
        , factory(f)
    {
    }

    EventLoopGroup& group;
    StreamClientOptions options; // 第一次 Start 时整个移进 Connector
    ReconnectPolicy policy;
    ConnectionHandler& handler;
    const FrameCodecFactory& factory;

    EventLoop* loop = nullptr; // 第一次 Start 时选定，此后固定
    std::unique_ptr<Connector> connector;
    EventLoop::HookId hook = 0; // 只在 loop 线程访问
};

StreamClient::StreamClient(EventLoopGroup& group,
                           const Endpoint& endpoint,
                           StreamClientOptions options,
                           ReconnectPolicy policy,
                           ConnectionHandler& handler,
                           const FrameCodecFactory& factory)
    : endpoint_(endpoint)
    , impl_(std::make_unique<Impl>(group, std::move(options), policy, handler, factory))
{
}

StreamClient::~StreamClient()
{
    Impl& m = *impl_;
    if (m.connector == nullptr || !m.group.running())
        return; // 没 Start 过；或 group 已 Shutdown，停机钩子已经处理过
    // 同步停：不然 Stop 投递的任务可能在本对象销毁之后才在 loop 上跑。
    (void)m.loop->CallSync(
        [&m]
        {
            m.connector->Stop();
            if (m.hook != 0)
            {
                m.loop->RemoveShutdownHook(m.hook);
                m.hook = 0;
            }
            return 0;
        });
    // 被 Stop 关掉的连接要到本轮分发之后才回调 OnClosed（回调经 Connector 里的转发器）：等它跑完再销毁。
    for (int i = 0; i < 5000 && m.connector->awaiting_closed(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

int StreamClient::Start()
{
    Impl& m = *impl_;
    if (!m.group.running())
    {
        // 回调里 Start()（文档允许）可能恰好撞上停机：返回 ESHUTDOWN，不是编程错误。
        if (m.group.stopping())
            return ESHUTDOWN;
        LOG_FATAL("StreamClient::Start 之前必须已 EventLoopGroup::Start() 成功 endpoint={}", endpoint_.ToString());
    }
    if (m.connector == nullptr)
    {
        if (m.options.connect_timeout.count() <= 0 || !m.policy.Valid())
            return EINVAL;
        m.loop = &GroupAccess::PickLoop(m.group);
        m.connector = std::make_unique<Connector>(
            *m.loop, endpoint_, std::move(m.options), m.policy, m.handler, m.factory, handle_);
    }
    m.loop->Post(
        [&m]
        {
            if (m.hook == 0) // 停机时 loop 自己停掉重连：group 不认识 StreamClient，只认钩子
            {
                m.hook = m.loop->AddShutdownHook(
                    [&m]
                    {
                        m.hook = 0; // 钩子执行完就被清掉了
                        m.connector->Detach();
                    });
            }
            m.connector->Start();
        });
    return 0;
}

void StreamClient::Stop()
{
    Impl& m = *impl_;
    if (m.connector == nullptr)
        return;
    m.loop->Post([&m] { m.connector->Stop(); });
}

SendResult StreamClient::Send(SharedBuffer payload)
{
    const ConnectionHandle h = handle();
    if (!h.valid())
        return SendResult::Closed;
    return h.Send(std::move(payload));
}

} // namespace breeze
