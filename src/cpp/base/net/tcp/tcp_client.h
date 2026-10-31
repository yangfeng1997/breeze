// TCP 客户端：连接一个 TcpEndpoint 并自动重连。与 UdsClient 并列，共用 StreamClient 里的重连 / 超时 / 状态逻辑；
// 区别只在类型化的端点与选项：TCP 专有的 keepalive 等放在 TcpClientOptions::tcp，UDS 的选项里没有它们。
// 生命周期、Start / Stop / Send 的约定见 stream/stream_client.h。

#pragma once

#include <chrono>

#include "stream_client.h"
#include "tcp_endpoint.h"
#include "tcp_options.h"

namespace breeze
{

struct TcpClientOptions
{
    ConnectionOptions connection; // 水位、idle_timeout
    TcpOptions tcp;               // keepalive、TCP_USER_TIMEOUT
    // 非阻塞 connect 的超时，到点按失败（ETIMEDOUT）重试；否则连到黑洞地址要等内核 SYN 重试约 2 分钟。
    std::chrono::milliseconds connect_timeout{5000};
    ConnectFailedCallback on_connect_failed; // 空 = 不通知（仍会记日志）
};

class TcpClient final : public StreamClient
{
public:
    TcpClient(EventLoopGroup& group,
              const TcpEndpoint& peer,
              TcpClientOptions options,
              ReconnectPolicy policy,
              ConnectionHandler& handler,
              const FrameCodecFactory& factory);

    // 对端地址（构造时给的）。
    TcpEndpoint peer() const;
};

} // namespace breeze
