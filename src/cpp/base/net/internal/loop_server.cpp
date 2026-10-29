#include "loop_server.h"

#include "connection.h"
#include "logger.h"
#include "logger_macros.h"
#include "peer_info.h"

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
        LOG_WARN("连接表已满（{}），拒绝新连接", loop_.table().capacity()); // fd 随之关闭，没有任何回调
        return;
    }
    PeerInfo info;
    info.endpoint = std::move(peer);

    // 差异只在建立时：TCP 设选项，UDS 取对端进程身份。Connection 以上不出现 if tcp。
    if (acceptor_.endpoint().kind() == Endpoint::Kind::Tcp)
    {
        if (const int e = sock::SetNoDelay(fd.get()); e != 0)
            LOG_WARN("设置 TCP_NODELAY 失败 errno={}", e);
        if (const int e = sock::SetKeepAlive(fd.get(), KeepAlive{}); e != 0)
            LOG_WARN("设置 keepalive 失败 errno={}", e);
    }
    else
    {
        PeerCredentials cred;
        if (sock::GetPeerCredentials(fd.get(), cred) == 0)
            info.credentials = cred;
    }

    Connection* raw = loop_.NewConnection(std::move(fd), std::move(info), handler_, factory_.Create(), options_);
    (void)raw->Establish(); // 失败时连接已被标记关闭、没有任何回调，回收时销毁
}

} // namespace breeze
