#include "base/net/uds/uds_client.h"

#include <utility>

namespace breeze
{

namespace
{

StreamClientOptions ToStreamOptions(UdsClientOptions&& o)
{
    StreamClientOptions s;
    s.connection = o.connection;
    s.connect_timeout = o.connect_timeout;
    s.on_connect_failed = std::move(o.on_connect_failed);
    return s; // s.tcp 保持默认：UDS 连接不会去读它
}

} // namespace

UdsClient::UdsClient(EventLoopGroup& group,
                     const UdsEndpoint& peer,
                     UdsClientOptions options,
                     ReconnectPolicy policy,
                     IConnectionHandler& handler,
                     const IFrameCodecFactory& factory)
    : StreamClient(group, peer.endpoint(), ToStreamOptions(std::move(options)), policy, handler, factory)
{
}

UdsEndpoint UdsClient::peer() const
{
    // 基类的端点由构造时的 UdsEndpoint 而来，种类不会变，所以转换一定成功。
    return *UdsEndpoint::From(endpoint());
}

} // namespace breeze
