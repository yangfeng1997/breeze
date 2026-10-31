#include "net_module.h"

#include <cerrno>
#include <cstring>
#include <format>
#include <optional>

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
    o.tcp.keepalive_idle = std::chrono::seconds(c.keepaliveIdleS);
    o.tcp.keepalive_interval = std::chrono::seconds(c.keepaliveIntervalS);
    o.tcp.keepalive_count = static_cast<int>(c.keepaliveCount);
    o.tcp.tcp_user_timeout = std::chrono::milliseconds(c.tcpUserTimeoutMs);
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
    StreamServerOptions so;
    so.connection = options_.connection;
    so.tcp = options_.tcp;
    return AddServer(endpoint, handler, factory, std::move(so));
}

Result<NetServer*> NetModule::AddServer(std::string_view endpoint,
                                        MainLoopHandler& handler,
                                        const FrameCodecFactory& factory,
                                        StreamServerOptions options)
{
    if (auto r = CheckPhase("AddServer"); !r)
        return std::unexpected(r.error());
    const auto ep = Endpoint::Parse(endpoint);
    if (!ep)
        return Fail(
            Errc::Module,
            std::format(
                "net: 监听端点 '{}' 不合法（tcp://ip:port | udp://ip:port | unix:///path | unix-abstract://name）",
                endpoint));
    if (const auto udp = RudpEndpoint::From(*ep))
    {
        RudpServerOptions o;
        o.connection = options.connection;
        o.rudp = options_.rudp;
        o.accept_filter = std::move(options.accept_filter);
        auto s = AddRudpServer(*udp, handler, std::move(o));
        if (!s)
            return std::unexpected(s.error());
        return static_cast<NetServer*>(*s); // factory 与 options.tcp / backlog 对 rudp 没有意义，丢掉
    }
    const auto tcp = TcpEndpoint::From(*ep);
    const auto uds = tcp ? std::optional<UdsEndpoint>{} : UdsEndpoint::From(*ep);
    if (!tcp && !uds) // Endpoint 新增了种类而这里没跟上：明确报错，不要去解引用空的 optional
        return Fail(Errc::Module, std::format("net: 监听端点 '{}' 的传输种类还没有对应的服务端", endpoint));
    ConnectionHandler& wrapped = executor_.Wrap(handler);
    if (tcp)
    {
        TcpServerOptions o;
        o.connection = options.connection;
        o.tcp = options.tcp;
        o.accept_filter = std::move(options.accept_filter);
        o.backlog = options.backlog;
        servers_.push_back(std::make_unique<TcpServer>(*group_, *tcp, std::move(o), wrapped, factory));
    }
    else
    {
        // options.tcp 对 UDS 没有意义，这里丢掉。
        UdsServerOptions o;
        o.connection = options.connection;
        o.accept_filter = std::move(options.accept_filter);
        o.backlog = options.backlog;
        servers_.push_back(std::make_unique<UdsServer>(*group_, *uds, std::move(o), wrapped, factory));
    }
    return servers_.back().get();
}

Result<NetClient*> NetModule::AddClient(std::string_view endpoint,
                                        MainLoopHandler& handler,
                                        const FrameCodecFactory& factory)
{
    StreamClientOptions co;
    co.connection = options_.connection;
    co.tcp = options_.tcp;
    return AddClient(endpoint, handler, factory, std::move(co), ReconnectPolicy{});
}

Result<NetClient*> NetModule::AddClient(std::string_view endpoint,
                                        MainLoopHandler& handler,
                                        const FrameCodecFactory& factory,
                                        StreamClientOptions options,
                                        ReconnectPolicy policy)
{
    if (auto r = CheckPhase("AddClient"); !r)
        return std::unexpected(r.error());
    const auto ep = Endpoint::Parse(endpoint);
    if (!ep)
        return Fail(
            Errc::Module,
            std::format(
                "net: 对端端点 '{}' 不合法（tcp://ip:port | udp://ip:port | unix:///path | unix-abstract://name）",
                endpoint));
    if (RudpEndpoint::From(*ep))
        return Fail(Errc::Module,
                    std::format("net: 对端端点 '{}' 是 rudp（udp）：客户端要凭控制面发的票据握手，请用 AddRudpClient",
                                endpoint));
    const auto tcp = TcpEndpoint::From(*ep);
    const auto uds = tcp ? std::optional<UdsEndpoint>{} : UdsEndpoint::From(*ep);
    if (!tcp && !uds)
        return Fail(Errc::Module, std::format("net: 对端端点 '{}' 的传输种类还没有对应的客户端", endpoint));
    ConnectionHandler& wrapped = executor_.Wrap(handler);
    if (tcp)
    {
        TcpClientOptions o;
        o.connection = options.connection;
        o.tcp = options.tcp;
        o.connect_timeout = options.connect_timeout;
        o.on_connect_failed = std::move(options.on_connect_failed);
        clients_.push_back(std::make_unique<TcpClient>(*group_, *tcp, std::move(o), policy, wrapped, factory));
    }
    else
    {
        // options.tcp 对 UDS 没有意义，这里丢掉。
        UdsClientOptions o;
        o.connection = options.connection;
        o.connect_timeout = options.connect_timeout;
        o.on_connect_failed = std::move(options.on_connect_failed);
        clients_.push_back(std::make_unique<UdsClient>(*group_, *uds, std::move(o), policy, wrapped, factory));
    }
    return clients_.back().get();
}

Result<RudpServer*> NetModule::AddRudpServer(const RudpEndpoint& endpoint, MainLoopHandler& handler)
{
    RudpServerOptions o;
    o.connection = options_.connection;
    o.rudp = options_.rudp;
    return AddRudpServer(endpoint, handler, std::move(o));
}

Result<RudpServer*> NetModule::AddRudpServer(const RudpEndpoint& endpoint,
                                             MainLoopHandler& handler,
                                             RudpServerOptions options)
{
    if (auto r = CheckPhase("AddRudpServer"); !r)
        return std::unexpected(r.error());
    if (!options.rudp.Valid())
        return Fail(Errc::Module,
                    std::format("net: rudp 服务端 {} 的传输参数不合法（见 RudpOptions::Valid）", endpoint.ToString()));
    ConnectionHandler& wrapped = executor_.Wrap(handler);
    auto server = std::make_unique<RudpServer>(*group_, endpoint, std::move(options), wrapped);
    RudpServer* raw = server.get();
    servers_.push_back(std::move(server));
    return raw;
}

Result<RudpClient*> NetModule::AddRudpClient(const RudpEndpoint& server, MainLoopHandler& handler)
{
    RudpClientOptions o;
    o.connection = options_.connection;
    o.rudp = options_.rudp;
    return AddRudpClient(server, handler, std::move(o));
}

Result<RudpClient*> NetModule::AddRudpClient(const RudpEndpoint& server,
                                             MainLoopHandler& handler,
                                             RudpClientOptions options)
{
    if (auto r = CheckPhase("AddRudpClient"); !r)
        return std::unexpected(r.error());
    if (!options.rudp.Valid())
        return Fail(Errc::Module,
                    std::format("net: rudp 客户端 {} 的传输参数不合法（见 RudpOptions::Valid）", server.ToString()));
    ConnectionHandler& wrapped = executor_.Wrap(handler);
    auto client = std::make_unique<RudpClient>(*group_, server, std::move(options), wrapped);
    RudpClient* raw = client.get();
    rudp_clients_.push_back(std::move(client));
    return raw;
}

// Module::Start 的约定：返回错误的模块必须自清理。已启动的 server / client 先停，再 Shutdown group。
void NetModule::Rollback()
{
    for (auto& s : servers_)
        s->Stop();
    for (auto& c : clients_)
        c->Stop();
    for (auto& c : rudp_clients_)
        c->Stop(); // 业务可能已经 Start 过（Start 之后装配的话）；没 Start 过是空操作
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
    // 等连接关闭期间继续消费事件：连接进入优雅关闭后暂停读不再生效（要读到对端的 FIN 与最后几条消息），
    // 对端还在灌数据的话，主线程只阻塞等着，事件就无界地堆在队列里。
    group_->Shutdown(options_.shutdown_deadline, [this] { return executor_.Drain(options_.wake_budget); });
    // IO 线程都已 join，不会再有新事件；把剩下的（含每条连接的 OnClosed）交给业务。
    while (executor_.Drain(options_.wake_budget))
    {
    }
}

} // namespace breeze
