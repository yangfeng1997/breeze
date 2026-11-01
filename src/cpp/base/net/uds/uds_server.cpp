#include "base/net/uds/uds_server.h"

#include <utility>

namespace breeze
{

namespace
{

StreamServerOptions ToStreamOptions(UdsServerOptions&& o)
{
    StreamServerOptions s;
    s.connection = o.connection;
    s.accept_filter = std::move(o.accept_filter);
    s.backlog = o.backlog;
    return s; // s.tcp 保持默认：UDS 连接不会去读它
}

} // namespace

UdsServer::UdsServer(EventLoopGroup& group,
                     const UdsEndpoint& endpoint,
                     UdsServerOptions options,
                     IConnectionHandler& handler,
                     const IFrameCodecFactory& factory)
    : StreamServer(group, endpoint.endpoint(), ToStreamOptions(std::move(options)), handler, factory)
{
}

UdsEndpoint UdsServer::local_endpoint() const
{
    // 基类的端点由构造时的 UdsEndpoint 而来，种类不会变，所以转换一定成功。
    return *UdsEndpoint::From(endpoint());
}

} // namespace breeze
