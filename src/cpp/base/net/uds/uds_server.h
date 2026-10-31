// UDS 服务端：监听一个 UdsEndpoint（路径或抽象名）。与 TcpServer 并列，共用 StreamServer 里的 accept / 分配 loop /
// 过滤逻辑；UDS 的选项里没有 TCP 专有的 keepalive 等，AcceptFilter 看到的 PeerInfo 带 credentials（uid / gid / pid）。
// 生命周期、Start / Stop 的约定见 stream/stream_server.h。

#pragma once

#include <sys/socket.h>

#include "base/net/stream/stream_server.h"
#include "base/net/uds/uds_endpoint.h"

namespace breeze
{

struct UdsServerOptions
{
    ConnectionOptions connection; // 水位、idle_timeout：对每条被接受的连接生效
    AcceptFilter accept_filter;   // 空 = 全部接受；PeerInfo::credentials 是对端进程的 uid / gid / pid，可据此校验身份
    int backlog = SOMAXCONN;      // 内核还会按 net.core.somaxconn 截断
};

class UdsServer final : public StreamServer
{
public:
    UdsServer(EventLoopGroup& group,
              const UdsEndpoint& endpoint,
              UdsServerOptions options,
              IConnectionHandler& handler,
              const IFrameCodecFactory& factory);

    // 实际绑定的地址（与构造时给的相同）。
    UdsEndpoint local_endpoint() const;
};

} // namespace breeze
