#include "loop_server.h"

#include "connection.h"
#include "logger.h"
#include "logger_macros.h"
#include "peer_info.h"
#include "transport.h"

namespace breeze
{

LoopServer::LoopServer(EventLoop& loop,
                       Endpoint endpoint,
                       ConnectionHandler& handler,
                       const FrameCodecFactory& factory,
                       ConnectionOptions options)
    : loop_(loop)
    , handler_(handler)
    , factory_(factory)
    , options_(options)
    , acceptor_(loop,
                std::move(endpoint),
                [this](Fd fd, std::optional<Endpoint> peer) { OnAccept(std::move(fd), std::move(peer)); })
{
}

void LoopServer::OnAccept(Fd fd, std::optional<Endpoint> peer)
{
    if (!loop_.table().TryReserve())
    {
        LoopStats::Bump(loop_.stats().rejected_full);
        LOG_WARN("连接表已满（{}），拒绝新连接", loop_.table().capacity()); // fd 随之关闭，没有任何回调
        return;
    }
    PeerInfo info;
    info.endpoint = std::move(peer);

    SetupTransport(fd.get(), acceptor_.endpoint().kind(), options_, info); // 差异只在建立时

    Connection* raw = loop_.NewConnection(std::move(fd), std::move(info), handler_, factory_.Create(), options_);
    (void)raw->Establish(); // 失败时连接已被标记关闭、没有任何回调，回收时销毁
}

} // namespace breeze
