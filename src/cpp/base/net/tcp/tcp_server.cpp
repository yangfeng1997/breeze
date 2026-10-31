#include "tcp_server.h"

#include <utility>

namespace breeze
{

namespace
{

StreamServerOptions ToStreamOptions(TcpServerOptions&& o)
{
    StreamServerOptions s;
    s.connection = o.connection;
    s.tcp = o.tcp;
    s.accept_filter = std::move(o.accept_filter);
    s.backlog = o.backlog;
    return s;
}

} // namespace

TcpServer::TcpServer(EventLoopGroup& group,
                     const TcpEndpoint& endpoint,
                     TcpServerOptions options,
                     ConnectionHandler& handler,
                     const FrameCodecFactory& factory)
    : StreamServer(group, endpoint.endpoint(), ToStreamOptions(std::move(options)), handler, factory)
{
}

TcpEndpoint TcpServer::local_endpoint() const
{
    // 基类的端点由构造时的 TcpEndpoint 而来，绑定后只是填上实际端口，种类不会变，所以转换一定成功。
    return *TcpEndpoint::From(endpoint());
}

} // namespace breeze
