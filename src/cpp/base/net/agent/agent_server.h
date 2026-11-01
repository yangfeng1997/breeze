// Agent 一侧的整套服务端：监听一个 UDS，App 经共享内存或仅 UDS 接入（握手时协商，见 shm_channel_server.h），
// 消息交给 AgentRouter 解析信封、核对身份、判过期、查路由表、转发（见 agent_router.h）。
// 实现 INetServer：NetModule 经它持有，随模块启停。设计见 docs/共享内存IPC设计.md §6.1 / §10。
//
// **EventLoopGroup 只能有一个 loop（io_threads = 1）**：路由器与路由表是单线程访问的，通道、握手、仅 UDS 的连接、
// 以及 Agent 之间的链路（用 router() 当 IConnectionHandler）的回调都必须在同一个线程上。Start 在 group 有多个 loop 时返回 EINVAL。
// Agent 之间的链路用 router() 当处理器接进来（NetModule::AddIoServer / AddIoClient），再把"远端 Node 经哪条链路"登记进路由表。

#pragma once

#include <cstddef>

#include "base/net/agent/agent_router.h"
#include "base/net/agent/route_table.h"
#include "base/net/codec/frame_codec_interface.h"
#include "base/net/codec/length_prefix_codec.h"
#include "base/net/connection/connection_options.h"
#include "base/net/connection/net_server_interface.h"
#include "base/net/endpoint/endpoint.h"
#include "base/net/loop/event_loop_group.h"
#include "base/net/shm/shm_channel_server.h"

namespace breeze::agent
{

struct AgentServerOptions
{
    ShmServerOptions shm;       // 共享内存通道的配额、保留、超时（allow_shm 在里面）
    ConnectionOptions connection; // 仅 UDS 的对端的连接选项
    // 仅 UDS 的数据通道的帧格式：不给就用 LengthPrefixCodec，payload 上限 max_frame。App 那边（AgentClientOptions::codec）要一致。
    const IFrameCodecFactory* codec = nullptr;
    std::size_t max_frame = 1 << 20;
    AgentRouterOptions router;
};

class AgentServer final : public INetServer
{
public:
    // table 只持引用，要比本对象活得久；只在路由器所在的 loop 线程上访问。
    AgentServer(EventLoopGroup& group, const Endpoint& endpoint, RouteTable& table, AgentServerOptions options = {});
    ~AgentServer() override;

    // 0 = 成功；EINVAL = group 有多个 loop；其余见 ShmChannelServer::Start。
    [[nodiscard]] int Start() override;
    void Stop() override { server_.Stop(); }
    const Endpoint& endpoint() const noexcept override { return server_.endpoint(); }

    AgentRouter& router() noexcept { return router_; }
    ShmServerStats shm_stats() const noexcept { return server_.stats(); }

private:
    EventLoopGroup& group_;
    LengthPrefixCodecFactory default_codec_;
    const IFrameCodecFactory* codec_;
    ConnectionOptions connection_;
    AgentRouter router_;
    ShmChannelServer server_;
};

} // namespace breeze::agent
