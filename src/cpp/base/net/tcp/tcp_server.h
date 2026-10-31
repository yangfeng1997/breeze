// TCP 服务端：监听一个 TcpEndpoint。与 UdsServer 并列，共用 StreamServer 里的 accept / 分配 loop / 过滤逻辑；
// 区别只在类型化的端点与选项：TCP 专有的 keepalive 等放在 TcpServerOptions::tcp，UDS 的选项里没有它们。
// 生命周期、Start / Stop 的约定见 stream/stream_server.h。

#pragma once

#include <sys/socket.h>

#include "base/net/stream/stream_server.h"
#include "base/net/tcp/tcp_endpoint.h"
#include "base/net/tcp/tcp_options.h"

namespace breeze
{

struct TcpServerOptions
{
    ConnectionOptions connection; // 水位、idle_timeout：对每条被接受的连接生效
    TcpOptions tcp;               // keepalive、TCP_USER_TIMEOUT
    AcceptFilter accept_filter;   // 空 = 全部接受；PeerInfo::endpoint 是对端的 IP 与端口，没有 credentials
    int backlog = SOMAXCONN;      // 内核还会按 net.core.somaxconn 截断
};

class TcpServer final : public StreamServer
{
public:
    TcpServer(EventLoopGroup& group,
              const TcpEndpoint& endpoint,
              TcpServerOptions options,
              IConnectionHandler& handler,
              const IFrameCodecFactory& factory);

    // 实际绑定的地址：端口填 0 时，Start 成功之后这里是系统分配的端口。Start 之前就是构造时给的端点。
    TcpEndpoint local_endpoint() const;
};

} // namespace breeze
