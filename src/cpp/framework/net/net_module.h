// 网络模块：装配 EventLoopGroup、NetServer / NetClient 与 MainLoopExecutor，把连接事件回投 App 主循环。
// 设计见 docs/网络库设计.md §5。这里只做装配，不含 IO 逻辑。
//
// 用法：业务模块 DependsOn("net")，在自己的 Init 里 AddServer / AddClient（传 MainLoopHandler 与 FrameCodecFactory）；
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
#include <memory>
#include <string_view>
#include <vector>

#include "config/net_config.h"
#include "connection_options.h"
#include "event_loop_group.h"
#include "main_loop_executor.h"
#include "main_loop_handler.h"
#include "module.h"
#include "net_client.h"
#include "net_server.h"

namespace breeze
{

struct NetModuleOptions
{
    EventLoopGroupOptions group;
    ConnectionOptions connection; // AddServer / AddClient 不另给选项时用它
    InboundLimits inbound;
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
    // unix-abstract://name（配置里直接写字符串）；不合法返回错误并带上原字符串。返回的指针在模块存活期内有效。
    // 之后经 NetClient::Send / ConnectionHandle 发送；handler 与 factory 要比 Shutdown 活得久。
    Result<NetServer*> AddServer(std::string_view endpoint, MainLoopHandler& handler, const FrameCodecFactory& factory);
    Result<NetServer*> AddServer(std::string_view endpoint,
                                 MainLoopHandler& handler,
                                 const FrameCodecFactory& factory,
                                 ServerOptions options);
    Result<NetClient*> AddClient(std::string_view endpoint, MainLoopHandler& handler, const FrameCodecFactory& factory);
    Result<NetClient*> AddClient(std::string_view endpoint,
                                 MainLoopHandler& handler,
                                 const FrameCodecFactory& factory,
                                 ClientOptions options,
                                 ReconnectPolicy policy);

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

    const bool from_config_;
    NetModuleOptions options_;
    Phase phase_ = Phase::New;

    // 销毁顺序（与声明相反）：server / client → group → executor。server / client 的析构在 group
    // 仍运行时会同步停掉自己。
    MainLoopExecutor executor_;
    std::unique_ptr<EventLoopGroup> group_;
    std::vector<std::unique_ptr<NetServer>> servers_;
    std::vector<std::unique_ptr<NetClient>> clients_;
};

} // namespace breeze
