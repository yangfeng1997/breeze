#include "tcp_client.h"

#include <utility>

namespace breeze
{

namespace
{

StreamClientOptions ToStreamOptions(TcpClientOptions&& o)
{
    StreamClientOptions s;
    s.connection = o.connection;
    s.tcp = o.tcp;
    s.connect_timeout = o.connect_timeout;
    s.on_connect_failed = std::move(o.on_connect_failed);
    return s;
}

} // namespace

TcpClient::TcpClient(EventLoopGroup& group,
                     const TcpEndpoint& peer,
                     TcpClientOptions options,
                     ReconnectPolicy policy,
                     ConnectionHandler& handler,
                     const FrameCodecFactory& factory)
    : StreamClient(group, peer.endpoint(), ToStreamOptions(std::move(options)), policy, handler, factory)
{
}

TcpEndpoint TcpClient::peer() const
{
    // 基类的端点由构造时的 TcpEndpoint 而来，种类不会变，所以转换一定成功。
    return *TcpEndpoint::From(endpoint());
}

} // namespace breeze
