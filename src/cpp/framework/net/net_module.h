// 网络模块：装配 EventLoopGroup、各传输的服务端 / 客户端（经 INetServer / INetClient 接口持有）与 MainLoopExecutor，
// 把连接事件回投 App 主循环。
// 设计见 docs/网络库设计.md §5。这里只做装配，不含 IO 逻辑。
//
// 用法：业务模块 DependsOn("net")，在 Init 里 AddServer / AddClient（传 IMainLoopHandler 与 IFrameCodecFactory）；
// NetModule::Start 起 group，再依次起 server 与 client。IO 线程把事件入队后 App::Notify()，主循环调 Wake()，Wake =
// Drain(budget)。
//
// 两种构造：
//   NetModule(app)           Init 时从 ConfigModule 读 `net` 段（config/schema/net.proto），DependsOn("config")
//   NetModule(app, options)  直接给选项（测试，或不走配置的进程），不依赖 config
//
// **停机由 main 负责**：App 只管启动，Run 返回之后 main 要调 Shutdown()——group 优雅关闭，再把事件 Drain 到空，
// 业务才看得到每条连接的 OnClosed（设计不变量 2）；之后才能销毁 App。没调 Shutdown 就销毁只做兜底：直接停 IO 线程，
// 未消费的事件丢弃、不回调业务。
// 日志部署建议：网络库所在进程把日志 Overflow 配成 DropOldest——默认 Block，磁盘变慢会卡住 IO 线程。

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/net/agent/agent_client.h"
#include "base/net/agent/agent_server.h"
#include "base/net/connection/connection_options.h"
#include "base/net/connection/net_client_interface.h"
#include "base/net/connection/net_server_interface.h"
#include "base/net/loop/event_loop_group.h"
#include "base/net/rudp/rudp_client.h"
#include "base/net/rudp/rudp_options.h"
#include "base/net/rudp/rudp_server.h"
#include "base/net/stream/stream_client.h"
#include "base/net/stream/stream_server.h"
#include "base/net/tcp/tcp_client.h"
#include "base/net/tcp/tcp_options.h"
#include "base/net/tcp/tcp_server.h"
#include "base/net/uds/uds_client.h"
#include "base/net/uds/uds_server.h"
#include "framework/app/module.h"
#include "framework/net/main_loop_executor.h"
#include "framework/net/main_loop_handler_interface.h"
#include "gen/config/net_config.h"

namespace breeze
{

// 本机 Agent（Sidecar）：App 一侧 AddAgentClient 用（配置里的 net.agent_*）。
struct AgentLinkOptions
{
    std::string endpoint = "unix:///run/breeze/agent.sock";
    // 是否启用共享内存。true：握手时要共享内存，Agent 不提供或给不了时自动退化成仅 UDS；false：只用 UDS。
    bool shm_enabled = true;
    // 共享内存每条环的容量（字节，2 的幂）；0 = 默认。
    std::uint64_t ring_capacity = 0;
};

struct NetModuleOptions
{
    EventLoopGroupOptions group;
    ConnectionOptions connection; // AddServer / AddClient 不另给选项时用它
    TcpOptions tcp;               // 同上，只对 TCP 端点生效
    RUdpOptions rudp; // 同上，只对 udp:// 端点（rudp）生效；net.proto 里还没有 rudp 段，配置驱动的构造用代码里的默认值
    InboundLimits inbound;
    AgentLinkOptions agent;
    std::size_t wake_budget = 1024;
    std::chrono::milliseconds shutdown_deadline{5000};
};

// 校验 net.proto 的取值范围由配置库负责；这里只做字段搬运与单位换算。
NetModuleOptions NetModuleOptionsFrom(const config::NetConfig& cfg);

class NetModule final : public Module, public IWaker
{
public:
    explicit NetModule(App& app);
    NetModule(App& app, NetModuleOptions options);
    ~NetModule() override;

    std::string_view Name() const override { return "net"; }
    std::vector<std::string_view> DependsOn() const override;
    Result<> Init() override;
    Result<> Start() override;

    // = executor.Drain(wake_budget)：true 表示仍有积压，App 会补一个唤醒再转一轮。
    bool Wake() override;

    // 装配：Init 之后、Start 之前（通常在依赖 net 的模块的 Init 里）。endpoint 是 tcp://ip:port | unix:///path |
    // unix-abstract://name | udp://ip:port（配置里直接写字符串）；不合法返回错误并带上原字符串。
    // 返回的指针在模块存活期内有效。
    // 之后经 INetClient::Send / ConnectionHandle 发送；handler 与 factory 要比 Shutdown 活得久。
    // udp:// 的 AddServer 装配的是 rudp 服务端（用模块的 connection 与 rudp 默认选项；显式选项里只有 connection 与
    // accept_filter 对它有意义），factory 对它被忽略：rudp 的消息自带边界，没有 codec。要签发票据就要拿到 RUdpServer，
    // 用 AddRUdpServer（或对返回的 INetServer* 做 dynamic_cast）。
    // AddClient 不收 udp://：rudp 客户端要凭票据握手，用 AddRUdpClient。
    Result<INetServer*> AddServer(std::string_view endpoint,
                                  IMainLoopHandler& handler,
                                  const IFrameCodecFactory& factory);
    Result<INetServer*> AddServer(std::string_view endpoint,
                                  IMainLoopHandler& handler,
                                  const IFrameCodecFactory& factory,
                                  StreamServerOptions options);
    Result<INetClient*> AddClient(std::string_view endpoint,
                                  IMainLoopHandler& handler,
                                  const IFrameCodecFactory& factory);
    Result<INetClient*> AddClient(std::string_view endpoint,
                                  IMainLoopHandler& handler,
                                  const IFrameCodecFactory& factory,
                                  StreamClientOptions options,
                                  ReconnectPolicy policy);

    // ---- 本机 Agent（Sidecar）：设计见 docs/共享内存IPC设计.md ----
    // App 一侧：连本机 Agent，握手时协商用共享内存（+ UDS 探活）还是仅 UDS（见 agent/agent_client.h），连上之后表现成一条普通连接，
    // 业务回调照常经 IMainLoopHandler 在主循环线程上执行、经 ConnectionHandle 发送，共享内存与否对业务透明。随模块启动，断线自动重连 / Reattach。
    // 第一个重载：端点、是否启用共享内存（net.agent_shm_enabled，关了就是仅 UDS）、环容量取 NetModuleOptions::agent（net.agent_*）；
    // busid 是这个 App 在 Agent 上的通道名；factory 是仅 UDS（含退化）时数据通道的帧格式，要与 Agent 那边一致，要比 Shutdown 活得久。
    // 第二个重载：全部自己给（options.codec 是帧格式）。端点必须是 UDS。
    Result<agent::AgentClient*> AddAgentClient(std::string_view busid,
                                               IMainLoopHandler& handler,
                                               const IFrameCodecFactory& factory);
    Result<agent::AgentClient*> AddAgentClient(std::string_view endpoint,
                                               IMainLoopHandler& handler,
                                               agent::AgentClientOptions options);
    // Agent 一侧：监听 UDS，App 经共享内存或仅 UDS 接入，消息由路由器在 IO 线程内转发（**不经主循环**，见 agent/agent_router.h）。
    // **要求 net.io_threads = 1**（路由器与路由表是单线程访问的），否则返回错误。table 要比模块活得久，只在路由器所在的 loop 线程上访问。
    // 第一个重载的连接选项取 NetModuleOptions::connection。返回的 AgentServer 里有路由器（router()）与统计。
    Result<agent::AgentServer*> AddAgentServer(std::string_view endpoint, agent::RouteTable& table);
    Result<agent::AgentServer*> AddAgentServer(std::string_view endpoint,
                                               agent::RouteTable& table,
                                               agent::AgentServerOptions options);
    // 回调直接在 IO 线程上执行（不回投主循环）的服务端 / 客户端：Agent 之间的转发链路用，handler 是 AgentServer::router()。
    // 端点是 tcp:// 或 unix://（不收 udp://）。handler 与 factory 要比 Shutdown 活得久。
    Result<INetServer*> AddIoServer(std::string_view endpoint, IConnectionHandler& handler, const IFrameCodecFactory& factory);
    Result<INetClient*> AddIoClient(std::string_view endpoint,
                                   IConnectionHandler& handler,
                                   const IFrameCodecFactory& factory,
                                   ReconnectPolicy policy = {});

    // rudp（可靠 UDP，KCP 之上）：设计与准入模型见 rudp/rudp_server.h、rudp_client.h。
    // 服务端随模块启动；没给选项时用 NetModuleOptions 的 connection 与 rudp。rudp 选项不合法返回错误。
    Result<RUdpServer*> AddRUdpServer(const RUdpEndpoint& endpoint, IMainLoopHandler& handler);
    Result<RUdpServer*> AddRUdpServer(const RUdpEndpoint& endpoint,
                                      IMainLoopHandler& handler,
                                      RUdpServerOptions options);
    // 客户端**不随模块启动**：票据要等控制面发下来，业务拿到后（构造时给，或 SetTicket）自己调 Start()，
    // 每次断线都要新票据（不自动重连）。随 group 一起停。没给选项时用 NetModuleOptions 的 connection 与 rudp。
    Result<RUdpClient*> AddRUdpClient(const RUdpEndpoint& server, IMainLoopHandler& handler);
    Result<RUdpClient*> AddRUdpClient(const RUdpEndpoint& server, IMainLoopHandler& handler, RUdpClientOptions options);

    // main 在 App::Run 返回之后调：group 优雅关闭（到 shutdown_deadline 强关），再把事件 Drain 到空。
    // 在主循环线程（调 Run 的线程）上调用。对没 Start 成功的、以及重复调用都是空操作。
    void Shutdown();

    const NetModuleOptions& options() const noexcept { return options_; }
    EventLoopGroup& group() { return *group_; }
    MainLoopExecutor& executor() { return executor_; }

private:
    enum class Phase : std::uint8_t
    {
        New,
        Inited,
        Started,
        ShutDown,
    };

    Result<> CheckPhase(std::string_view what) const;
    void Rollback();
    // 装配 TCP / UDS 的服务端 / 客户端（handler 已经按需包装过）。AddServer / AddIoServer 与 AddClient / AddIoClient 共用。
    INetServer* MakeStreamServer(const std::optional<TcpEndpoint>& tcp,
                                const std::optional<UdsEndpoint>& uds,
                                IConnectionHandler& handler,
                                const IFrameCodecFactory& factory,
                                StreamServerOptions options);
    INetClient* MakeStreamClient(const std::optional<TcpEndpoint>& tcp,
                                const std::optional<UdsEndpoint>& uds,
                                IConnectionHandler& handler,
                                const IFrameCodecFactory& factory,
                                StreamClientOptions options,
                                ReconnectPolicy policy);

    const bool from_config_;
    NetModuleOptions options_;
    Phase phase_ = Phase::New;

    // 销毁顺序（与声明相反）：server / client → group → executor。server / client 的析构在 group
    // 仍运行时会同步停掉自己。
    MainLoopExecutor executor_;
    std::unique_ptr<EventLoopGroup> group_;
    std::vector<std::unique_ptr<INetServer>> servers_;
    std::vector<std::unique_ptr<INetClient>> clients_;
    std::vector<std::unique_ptr<RUdpClient>> rudp_clients_; // 不随模块启动，见 AddRUdpClient
};

} // namespace breeze
