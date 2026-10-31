// UDS 客户端：连接一个 UdsEndpoint 并自动重连。与 TcpClient 并列，共用 StreamClient 里的重连 / 超时 / 状态逻辑；
// UDS 的选项里没有 TCP 专有的 keepalive 等。对端 sidecar 还没创建 socket 文件（ENOENT）时按退避重试。
// 生命周期、Start / Stop / Send 的约定见 stream/stream_client.h。

#pragma once

#include <chrono>

#include "stream_client.h"
#include "uds_endpoint.h"

namespace breeze
{

struct UdsClientOptions
{
    ConnectionOptions connection; // 水位、idle_timeout
    // 非阻塞 connect 的超时，到点按失败（ETIMEDOUT）重试。
    std::chrono::milliseconds connect_timeout{5000};
    ConnectFailedCallback on_connect_failed; // 空 = 不通知（仍会记日志）
};

class UdsClient final : public StreamClient
{
public:
    UdsClient(EventLoopGroup& group,
              const UdsEndpoint& peer,
              UdsClientOptions options,
              ReconnectPolicy policy,
              ConnectionHandler& handler,
              const FrameCodecFactory& factory);

    // 对端地址（构造时给的）。
    UdsEndpoint peer() const;
};

} // namespace breeze
