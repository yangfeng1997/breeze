#include "net_module.h"

#include <cerrno>
#include <cstring>
#include <format>

#include "config_module.h"
#include "logger_macros.h"

namespace breeze
{

NetModuleOptions NetModuleOptionsFrom(const config::NetConfig& c)
{
    NetModuleOptions o;
    o.group.io_threads = c.ioThreads;
    o.group.max_connections_per_loop = c.maxConnectionsPerLoop;
    o.connection.high_water_mark = c.highWaterMarkBytes;
    o.connection.idle_timeout = std::chrono::milliseconds(c.idleTimeoutMs);
    o.connection.keepalive_idle = std::chrono::seconds(c.keepaliveIdleS);
    o.connection.keepalive_interval = std::chrono::seconds(c.keepaliveIntervalS);
    o.connection.keepalive_count = static_cast<int>(c.keepaliveCount);
    o.connection.tcp_user_timeout = std::chrono::milliseconds(c.tcpUserTimeoutMs);
    o.inbound.max_messages_per_conn = c.inboundMaxMessagesPerConn;
    o.inbound.max_bytes_per_conn = c.inboundMaxBytesPerConn;
    o.inbound.global_budget_bytes = static_cast<std::size_t>(c.inboundGlobalBudgetMb) * 1024 * 1024;
    o.wake_budget = c.wakeBudget;
    o.shutdown_deadline = std::chrono::milliseconds(c.shutdownDeadlineMs);
    return o;
}

NetModule::NetModule(App& app)
    : Module(app)
    , from_config_(true)
    , executor_(app)
{
}

NetModule::NetModule(App& app, NetModuleOptions options)
    : Module(app)
    , from_config_(false)
    , options_(std::move(options))
    , executor_(app)
{
}

NetModule::~NetModule() = default;

std::vector<std::string_view> NetModule::DependsOn() const
{
    if (from_config_)
        return {"config"};
    return {};
}

Result<> NetModule::Init()
{
    if (from_config_)
    {
        auto* cfg = app().GetModule<ConfigModule>("config");
        if (cfg == nullptr)
            return Fail(Errc::Module, "net: 找不到 config 模块（NetModule(app) 要从它读 net 段）");
        options_ = NetModuleOptionsFrom(cfg->Common().net);
    }
    executor_.SetLimits(options_.inbound);
    group_ = std::make_unique<EventLoopGroup>(options_.group);
    phase_ = Phase::Inited;
    LOG_INFO("net ready io_threads={} max_connections_per_loop={} wake_budget={}",
             options_.group.io_threads,
             options_.group.max_connections_per_loop,
             options_.wake_budget);
    return {};
}

Result<> NetModule::CheckPhase(std::string_view what) const
{
    if (phase_ != Phase::Inited)
        return Fail(Errc::Phase, std::format("net: {} 只能在 Init 之后、Start 之前调用", what));
    return {};
}

Result<NetServer*> NetModule::AddServer(std::string_view endpoint,
                                        MainLoopHandler& handler,
                                        const FrameCodecFactory& factory)
{
    ServerOptions so;
    so.connection = options_.connection;
    return AddServer(endpoint, handler, factory, std::move(so));
}

Result<NetServer*> NetModule::AddServer(std::string_view endpoint,
                                        MainLoopHandler& handler,
                                        const FrameCodecFactory& factory,
                                        ServerOptions options)
{
    if (auto r = CheckPhase("AddServer"); !r)
        return std::unexpected(r.error());
    const auto ep = Endpoint::Parse(endpoint);
    if (!ep)
        return Fail(
            Errc::Module,
            std::format("net: 监听端点 '{}' 不合法（tcp://ip:port | unix:///path | unix-abstract://name）", endpoint));
    servers_.push_back(std::make_unique<NetServer>(*group_, *ep, std::move(options), executor_.Wrap(handler), factory));
    return servers_.back().get();
}

Result<NetClient*> NetModule::AddClient(std::string_view endpoint,
                                        MainLoopHandler& handler,
                                        const FrameCodecFactory& factory)
{
    ClientOptions co;
    co.connection = options_.connection;
    return AddClient(endpoint, handler, factory, std::move(co), ReconnectPolicy{});
}

Result<NetClient*> NetModule::AddClient(std::string_view endpoint,
                                        MainLoopHandler& handler,
                                        const FrameCodecFactory& factory,
                                        ClientOptions options,
                                        ReconnectPolicy policy)
{
    if (auto r = CheckPhase("AddClient"); !r)
        return std::unexpected(r.error());
    const auto ep = Endpoint::Parse(endpoint);
    if (!ep)
        return Fail(
            Errc::Module,
            std::format("net: 对端端点 '{}' 不合法（tcp://ip:port | unix:///path | unix-abstract://name）", endpoint));
    clients_.push_back(
        std::make_unique<NetClient>(*group_, *ep, std::move(options), policy, executor_.Wrap(handler), factory));
    return clients_.back().get();
}

// Module::Start 的约定：返回错误的模块必须自清理。已启动的 server / client 先停，再 Shutdown group。
void NetModule::Rollback()
{
    for (auto& s : servers_)
        s->Stop();
    for (auto& c : clients_)
        c->Stop();
    group_->Shutdown(std::chrono::milliseconds::zero());
    phase_ = Phase::ShutDown;
}

Result<> NetModule::Start()
{
    if (phase_ != Phase::Inited)
        return Fail(Errc::Phase, "net: Start 之前必须先 Init");
    if (const int e = group_->Start(); e != 0)
        return Fail(Errc::Module,
                    std::format("net: 启动 EventLoopGroup 失败（io_threads={} max_connections_per_loop={}）: {}",
                                options_.group.io_threads,
                                options_.group.max_connections_per_loop,
                                std::strerror(e)));
    for (auto& s : servers_)
    {
        if (const int e = s->Start(); e != 0)
        {
            const std::string msg = std::format("net: 监听 {} 失败: {}", s->endpoint().ToString(), std::strerror(e));
            Rollback();
            return Fail(Errc::Module, msg);
        }
        LOG_INFO("net listening {}", s->endpoint().ToString());
    }
    for (auto& c : clients_)
    {
        if (const int e = c->Start(); e != 0)
        {
            const std::string msg =
                std::format("net: 启动对 {} 的连接失败: {}", c->endpoint().ToString(), std::strerror(e));
            Rollback();
            return Fail(Errc::Module, msg);
        }
    }
    phase_ = Phase::Started;
    return {};
}

bool NetModule::Wake() { return executor_.Drain(options_.wake_budget); }

void NetModule::Shutdown()
{
    if (phase_ != Phase::Started)
        return;
    phase_ = Phase::ShutDown;
    group_->Shutdown(options_.shutdown_deadline);
    // IO 线程都已 join，不会再有新事件；把剩下的（含每条连接的 OnClosed）交给业务。
    while (executor_.Drain(options_.wake_budget))
    {
    }
}

} // namespace breeze
