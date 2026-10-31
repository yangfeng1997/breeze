#include "loop_server.h"

#include "logger.h"
#include "logger_macros.h"
#include "peer_info.h"
#include "socket_config.h"
#include "stream_connection.h"

namespace breeze
{

LoopServer::LoopServer(EventLoop& loop,
                       Endpoint endpoint,
                       ConnectionHandler& handler,
                       const FrameCodecFactory& factory,
                       ConnectionOptions options,
                       TcpOptions tcp)
    : loop_(loop)
    , handler_(handler)
    , factory_(factory)
    , options_(options)
    , tcp_(tcp)
    , acceptor_(loop,
                std::move(endpoint),
                [this](UniqueFd fd, std::optional<Endpoint> peer) { OnAccept(std::move(fd), std::move(peer)); })
{
}

void LoopServer::OnAccept(UniqueFd fd, std::optional<Endpoint> peer)
{
    if (!loop_.table().TryReserve())
    {
        LoopStats::Bump(loop_.stats().rejected_full);
        LOG_WARN("连接表已满（{}），拒绝新连接", loop_.table().capacity()); // fd 随之关闭，没有任何回调
        return;
    }
    PeerInfo info;
    info.endpoint = std::move(peer);

    ConfigureSocket(fd.get(), acceptor_.endpoint().kind(), tcp_, info); // 差异只在建立时

    StreamConnection* raw =
        StreamConnection::Create(loop_, std::move(fd), std::move(info), handler_, factory_.Create(), options_);
    (void)raw->Establish(); // 失败时连接已被标记关闭、没有任何回调，回收时销毁
}

} // namespace breeze
