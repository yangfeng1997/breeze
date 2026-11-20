#include "framework/net/net_module.h"

#include <cerrno>
#include <cstring>
#include <fmt/format.h>
#include <optional>

#include "base/logger/logger_macros.h"
#include "base/net/ws/ws_codec.h"
#include "framework/kernel/config_module.h"

namespace breeze
{

namespace
{

// 带 scheme 的端点：scheme 由这里剥掉，剩下的交给 Endpoint::Parse。tls:// = TCP + TLS 层；ws:// / wss:// = TCP +（可选 TLS）
// + WebSocket codec，路径写在 URL 里。
struct ParsedEndpoint
{
    Endpoint base; // tcp / udp / unix 端点（ws / wss 已剥掉 scheme 与路径）
    bool ws = false;
    bool tls = false;
    std::string ws_path = "/"; // ws / wss：URL 里的路径
};

std::expected<ParsedEndpoint, Error> ParseEndpoint(std::string_view text)
{
    const auto bad = [text](std::string_view why)
    { return Fail(Errc::Module, fmt::format("net: 端点 '{}' 不合法（{}）", text, why)); };

    const std::size_t scheme_end = text.find("://");
    if (scheme_end == std::string_view::npos)
        return bad("要写 scheme：tcp:// | tls:// | ws:// | wss:// | unix:// | unix-abstract:// | udp://");
    const std::string_view scheme = text.substr(0, scheme_end);
    const std::string_view rest = text.substr(scheme_end + 3);

    if (scheme == "tcp" || scheme == "udp" || scheme == "unix" || scheme == "unix-abstract")
    {
        const auto ep = Endpoint::Parse(text);
        if (!ep)
            return bad("地址部分解析失败");
        return ParsedEndpoint{*ep};
    }
    if (scheme == "tls")
    {
        const auto ep = Endpoint::Parse("tcp://" + std::string(rest));
        if (!ep)
            return bad("TLS 只能叠在 TCP 上（tls://ip:port）");
        return ParsedEndpoint{*ep, false, true, {}};
    }
    if (scheme == "ws" || scheme == "wss")
    {
        const std::size_t slash = rest.find('/');
        const std::string_view authority = rest.substr(0, slash);
        const auto ep = Endpoint::Parse("tcp://" + std::string(authority));
        if (!ep)
            return bad("WebSocket 只能叠在 TCP 上（ws://ip:port[/path]）");
        ParsedEndpoint p{*ep, true, scheme == "wss", "/"};
        if (slash != std::string_view::npos)
        {
            const std::string_view path = rest.substr(slash);
            if (path.find_first_of("?#") != std::string_view::npos)
                return bad("路径里不要带 '?' / '#'（查询串不参与路由）");
            p.ws_path = std::string(path);
        }
        return p;
    }
    return bad("不认识的 scheme");
}

} // namespace

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
    o.agent.endpoint = c.agentEndpoint;
    o.agent.shm_enabled = c.agentShmEnabled;
    o.agent.ring_capacity = static_cast<std::uint64_t>(c.agentShmRingMb) << 20;
    o.tls.cert_chain_file = c.tls.certChainFile;
    o.tls.private_key_file = c.tls.privateKeyFile;
    o.tls.ca_file = c.tls.caFile;
    o.tls.server_name = c.tls.serverName;
    o.tls.verify_peer = c.tls.verifyPeer;
    o.tls.require_client_cert = c.tls.requireClientCert;
    o.tls.min_version_13 = c.tls.minVersion == config::TlsVersionConfig::Tls13;
    o.ws_server.max_header_bytes = c.ws.maxHeaderBytes;
    o.ws_server.max_message_bytes = c.ws.maxMessageBytes;
    o.ws_server.message_type = c.ws.messageType == config::WsMessageTypeConfig::Text ? WsMessageType::Text : WsMessageType::Binary;
    o.ws_server.validate_utf8 = c.ws.validateUtf8;
    o.ws_server.allowed_origins.assign(c.ws.allowedOrigins.begin(), c.ws.allowedOrigins.end());
    o.ws_client.host.clear(); // 端点里的 authority 才是 Host
    o.ws_client.max_header_bytes = c.ws.maxHeaderBytes;
    o.ws_client.max_message_bytes = c.ws.maxMessageBytes;
    o.ws_client.message_type = o.ws_server.message_type;
    o.ws_client.validate_utf8 = c.ws.validateUtf8;
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

std::expected<void, Error> NetModule::Init()
{
    if (from_config_)
    {
        auto* cfg = app().GetModule<ConfigModule>("config");
        if (cfg == nullptr)
            return Fail(Errc::Module, "net: 找不到 config 模块（NetModule(app) 要从它读 net 段）");
        options_ = NetModuleOptionsFrom(cfg->Common().net);
    }
    // TLS 上下文只建一次（证书不随连接变）。建失败就 Init 失败：不要让进程带着错误的证书跑起来。
    if (!options_.tls.cert_chain_file.empty() || !options_.tls.private_key_file.empty())
    {
        std::string err;
        tls_server_ = MakeTlsServerLayer(options_.tls, err);
        if (!tls_server_)
            return Fail(Errc::Module, fmt::format("net: TLS 服务端上下文创建失败（{}）：{}", options_.tls.cert_chain_file, err));
    }
    if (!options_.tls.server_name.empty())
    {
        std::string err;
        tls_client_ = MakeTlsClientLayer(options_.tls, err);
        if (!tls_client_)
            return Fail(Errc::Module, fmt::format("net: TLS 客户端上下文创建失败（{}）：{}", options_.tls.server_name, err));
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

std::expected<void, Error> NetModule::CheckPhase(std::string_view what) const
{
    if (phase_ != Phase::Inited)
        return Fail(Errc::Phase, fmt::format("net: {} 只能在 Init 之后、Start 之前调用", what));
    return {};
}

std::expected<INetServer*, Error> NetModule::AddServer(std::string_view endpoint, IMainLoopHandler& handler)
{
    StreamServerOptions so;
    so.connection = options_.connection;
    so.tcp = options_.tcp;
    return AddServer(endpoint, handler, std::move(so));
}

std::expected<INetServer*, Error> NetModule::AddServer(std::string_view endpoint,
                                                       IMainLoopHandler& handler,
                                                       StreamServerOptions options)
{
    if (auto r = CheckPhase("AddServer"); !r)
        return std::unexpected(r.error());
    auto parsed = ParseEndpoint(endpoint);
    if (!parsed)
        return std::unexpected(parsed.error());
    if (!parsed->ws)
        return Fail(Errc::Module,
                    fmt::format("net: 端点 '{}' 不带分帧，AddServer 需要 codec 工厂；只有 ws:// / wss:// 可以不给", endpoint));
    // 端点里的路径优先于选项里的。
    WsServerOptions wo = options_.ws_server;
    wo.path = parsed->ws_path;
    auto factory = std::make_unique<WsServerCodecFactory>(std::move(wo));
    IFrameCodecFactory& f = *factory;
    codec_factories_.push_back(std::move(factory));
    if (parsed->tls)
    {
        if (!options.tls)
            options.tls = tls_server_;
        if (!options.tls)
            return Fail(Errc::Module,
                        fmt::format("net: 端点 '{}' 是 wss://，但没有 TLS 服务端上下文（net.tls.cert_chain_file / private_key_file）",
                                    endpoint));
    }
    return AddServer(parsed->base.ToString(), handler, f, std::move(options));
}

std::expected<INetServer*, Error> NetModule::AddServer(std::string_view endpoint,
                                                       IMainLoopHandler& handler,
                                                       const IFrameCodecFactory& factory)
{
    StreamServerOptions so;
    so.connection = options_.connection;
    so.tcp = options_.tcp;
    return AddServer(endpoint, handler, factory, std::move(so));
}

std::expected<INetServer*, Error> NetModule::AddServer(std::string_view endpoint,
                                                       IMainLoopHandler& handler,
                                                       const IFrameCodecFactory& factory,
                                                       StreamServerOptions options)
{
    if (auto r = CheckPhase("AddServer"); !r)
        return std::unexpected(r.error());
    auto parsed = ParseEndpoint(endpoint);
    if (!parsed)
        return std::unexpected(parsed.error());
    if (parsed->ws)
        return Fail(Errc::Module,
                    fmt::format("net: 端点 '{}' 是 ws:// / wss://，自带分帧，不要再给 codec 工厂（用不带工厂的 AddServer）", endpoint));
    if (parsed->tls && !options.tls)
        options.tls = tls_server_;
    if (parsed->tls && !options.tls)
        return Fail(Errc::Module,
                    fmt::format("net: 端点 '{}' 是 tls://，但没有 TLS 服务端上下文（net.tls.cert_chain_file / private_key_file）", endpoint));
    const auto& ep = parsed->base;
    if (const auto udp = RUdpEndpoint::From(ep))
    {
        RUdpServerOptions o;
        o.connection = options.connection;
        o.rudp = options_.rudp;
        o.accept_filter = std::move(options.accept_filter);
        auto s = AddRUdpServer(*udp, handler, std::move(o));
        if (!s)
            return std::unexpected(s.error());
        return static_cast<INetServer*>(*s); // factory 与 options.tcp / backlog 对 rudp 没有意义，丢掉
    }
    const auto tcp = TcpEndpoint::From(ep);
    const auto uds = tcp ? std::optional<UdsEndpoint>{} : UdsEndpoint::From(ep);
    if (!tcp && !uds) // Endpoint 新增了种类而这里没跟上：明确报错，不要去解引用空的 optional
        return Fail(Errc::Module, fmt::format("net: 监听端点 '{}' 的传输种类还没有对应的服务端", endpoint));
    return MakeStreamServer(tcp, uds, executor_.Wrap(handler), factory, std::move(options));
}

INetServer* NetModule::MakeStreamServer(const std::optional<TcpEndpoint>& tcp,
                                       const std::optional<UdsEndpoint>& uds,
                                       IConnectionHandler& handler,
                                       const IFrameCodecFactory& factory,
                                       StreamServerOptions options)
{
    if (tcp)
    {
        TcpServerOptions o;
        o.connection = options.connection;
        o.tcp = options.tcp;
        o.tls = std::move(options.tls);
        o.accept_filter = std::move(options.accept_filter);
        o.backlog = options.backlog;
        servers_.push_back(std::make_unique<TcpServer>(*group_, *tcp, std::move(o), handler, factory));
    }
    else
    {
        // options.tcp 对 UDS 没有意义，这里丢掉。
        UdsServerOptions o;
        o.connection = options.connection;
        o.accept_filter = std::move(options.accept_filter);
        o.backlog = options.backlog;
        servers_.push_back(std::make_unique<UdsServer>(*group_, *uds, std::move(o), handler, factory));
    }
    return servers_.back().get();
}

std::expected<INetClient*, Error> NetModule::AddClient(std::string_view endpoint,
                                                       IMainLoopHandler& handler,
                                                       ReconnectPolicy policy)
{
    StreamClientOptions co;
    co.connection = options_.connection;
    co.tcp = options_.tcp;
    return AddClient(endpoint, handler, std::move(co), policy);
}

std::expected<INetClient*, Error> NetModule::AddClient(std::string_view endpoint,
                                                       IMainLoopHandler& handler,
                                                       StreamClientOptions options,
                                                       ReconnectPolicy policy)
{
    if (auto r = CheckPhase("AddClient"); !r)
        return std::unexpected(r.error());
    auto parsed = ParseEndpoint(endpoint);
    if (!parsed)
        return std::unexpected(parsed.error());
    if (!parsed->ws)
        return Fail(Errc::Module,
                    fmt::format("net: 端点 '{}' 不带分帧，AddClient 需要 codec 工厂；只有 ws:// / wss:// 可以不给", endpoint));
    WsClientOptions wo = options_.ws_client;
    wo.path = parsed->ws_path;
    if (wo.host.empty() && parsed->base.IsIp())
    { // Host 头用 URL 里的 authority（IPv6 加方括号）
        wo.host = parsed->base.IsIPv6() ? "[" + parsed->base.address() + "]:" + std::to_string(parsed->base.port())
                                        : parsed->base.address() + ":" + std::to_string(parsed->base.port());
    }
    auto factory = std::make_unique<WsClientCodecFactory>(std::move(wo));
    IFrameCodecFactory& f = *factory;
    codec_factories_.push_back(std::move(factory));
    if (parsed->tls)
    {
        if (!options.tls)
            options.tls = tls_client_;
        if (!options.tls)
            return Fail(Errc::Module,
                        fmt::format("net: 端点 '{}' 是 wss://，但没有 TLS 客户端上下文（net.tls.server_name）", endpoint));
    }
    return AddClient(parsed->base.ToString(), handler, f, std::move(options), policy);
}

std::expected<INetClient*, Error> NetModule::AddClient(std::string_view endpoint,
                                                       IMainLoopHandler& handler,
                                                       const IFrameCodecFactory& factory)
{
    StreamClientOptions co;
    co.connection = options_.connection;
    co.tcp = options_.tcp;
    return AddClient(endpoint, handler, factory, std::move(co), ReconnectPolicy{});
}

std::expected<INetClient*, Error> NetModule::AddClient(std::string_view endpoint,
                                                       IMainLoopHandler& handler,
                                                       const IFrameCodecFactory& factory,
                                                       StreamClientOptions options,
                                                       ReconnectPolicy policy)
{
    if (auto r = CheckPhase("AddClient"); !r)
        return std::unexpected(r.error());
    auto parsed = ParseEndpoint(endpoint);
    if (!parsed)
        return std::unexpected(parsed.error());
    if (parsed->ws)
        return Fail(Errc::Module,
                    fmt::format("net: 端点 '{}' 是 ws:// / wss://，自带分帧，不要再给 codec 工厂（用不带工厂的 AddClient）", endpoint));
    if (parsed->tls && !options.tls)
        options.tls = tls_client_;
    if (parsed->tls && !options.tls)
        return Fail(Errc::Module, fmt::format("net: 端点 '{}' 是 tls://，但没有 TLS 客户端上下文（net.tls.server_name）", endpoint));
    const auto& ep = parsed->base;
    if (RUdpEndpoint::From(ep))
        return Fail(Errc::Module,
                    fmt::format("net: 对端端点 '{}' 是 rudp（udp）：客户端要凭控制面发的票据握手，请用 AddRUdpClient",
                                endpoint));
    const auto tcp = TcpEndpoint::From(ep);
    const auto uds = tcp ? std::optional<UdsEndpoint>{} : UdsEndpoint::From(ep);
    if (!tcp && !uds)
        return Fail(Errc::Module, fmt::format("net: 对端端点 '{}' 的传输种类还没有对应的客户端", endpoint));
    return MakeStreamClient(tcp, uds, executor_.Wrap(handler), factory, std::move(options), policy);
}

INetClient* NetModule::MakeStreamClient(const std::optional<TcpEndpoint>& tcp,
                                       const std::optional<UdsEndpoint>& uds,
                                       IConnectionHandler& handler,
                                       const IFrameCodecFactory& factory,
                                       StreamClientOptions options,
                                       ReconnectPolicy policy)
{
    if (tcp)
    {
        TcpClientOptions o;
        o.connection = options.connection;
        o.tcp = options.tcp;
        o.tls = std::move(options.tls);
        o.connect_timeout = options.connect_timeout;
        o.on_connect_failed = std::move(options.on_connect_failed);
        clients_.push_back(std::make_unique<TcpClient>(*group_, *tcp, std::move(o), policy, handler, factory));
    }
    else
    {
        // options.tcp 对 UDS 没有意义，这里丢掉。
        UdsClientOptions o;
        o.connection = options.connection;
        o.connect_timeout = options.connect_timeout;
        o.on_connect_failed = std::move(options.on_connect_failed);
        clients_.push_back(std::make_unique<UdsClient>(*group_, *uds, std::move(o), policy, handler, factory));
    }
    return clients_.back().get();
}

// ---- 本机 Agent ----

std::expected<agent::AgentClient*, Error> NetModule::AddAgentClient(std::string_view busid,
                                                                    IMainLoopHandler& handler,
                                                                    const IFrameCodecFactory& factory)
{
    agent::AgentClientOptions o;
    o.busid = std::string(busid);
    o.transport = options_.agent.shm_enabled ? shm::wire::Transport::kShmPreferred : shm::wire::Transport::kUdsOnly;
    o.tx_capacity = options_.agent.ring_capacity;
    o.rx_capacity = options_.agent.ring_capacity;
    o.connection = options_.connection;
    o.codec = &factory;
    return AddAgentClient(options_.agent.endpoint, handler, std::move(o));
}

std::expected<agent::AgentClient*, Error> NetModule::AddAgentClient(std::string_view endpoint,
                                                                    IMainLoopHandler& handler,
                                                                    agent::AgentClientOptions options)
{
    if (auto r = CheckPhase("AddAgentClient"); !r)
        return std::unexpected(r.error());
    const auto ep = Endpoint::Parse(endpoint);
    if (!ep || (ep->kind() != Endpoint::Kind::Unix && ep->kind() != Endpoint::Kind::UnixAbstract))
        return Fail(Errc::Module,
                    fmt::format("net: Agent 端点 '{}' 不合法（必须是 unix:///path 或 unix-abstract://name：fd 只能经 UDS 传递）",
                                endpoint));
    if (!shm::wire::IsValidBusId(options.busid))
        return Fail(Errc::Module,
                    fmt::format("net: Agent 通道名 '{}' 不合法（[A-Za-z0-9._:@-]，1 到 63 个字符）", options.busid));
    if (!options.on_connect_failed)
    {
        const std::string target(endpoint);
        options.on_connect_failed = [target](int e, bool will_retry) noexcept
        { LOG_WARN("net: 连本机 Agent {} 失败: {} 重试={}", target, std::strerror(e), will_retry); };
    }
    IConnectionHandler& wrapped = executor_.Wrap(handler);
    auto client = std::make_unique<agent::AgentClient>(*group_, *ep, std::move(options), wrapped);
    agent::AgentClient* raw = client.get();
    clients_.push_back(std::move(client));
    return raw;
}

std::expected<agent::AgentServer*, Error> NetModule::AddAgentServer(std::string_view endpoint, agent::RouteTable& table)
{
    agent::AgentServerOptions o;
    o.connection = options_.connection;
    return AddAgentServer(endpoint, table, std::move(o));
}

std::expected<agent::AgentServer*, Error> NetModule::AddAgentServer(std::string_view endpoint,
                                                                    agent::RouteTable& table,
                                                                    agent::AgentServerOptions options)
{
    if (auto r = CheckPhase("AddAgentServer"); !r)
        return std::unexpected(r.error());
    if (options_.group.io_threads != 1)
        return Fail(Errc::Module,
                    fmt::format("net: AddAgentServer 要求 net.io_threads = 1（Agent 的路由器与路由表是单线程访问的），当前是 {}",
                                options_.group.io_threads));
    const auto ep = Endpoint::Parse(endpoint);
    if (!ep || (ep->kind() != Endpoint::Kind::Unix && ep->kind() != Endpoint::Kind::UnixAbstract))
        return Fail(Errc::Module,
                    fmt::format("net: Agent 监听端点 '{}' 不合法（必须是 unix:///path 或 unix-abstract://name）", endpoint));
    auto server = std::make_unique<agent::AgentServer>(*group_, *ep, table, std::move(options));
    agent::AgentServer* raw = server.get();
    servers_.push_back(std::move(server));
    return raw;
}

std::expected<INetServer*, Error> NetModule::AddIoServer(std::string_view endpoint,
                                                        IConnectionHandler& handler,
                                                        const IFrameCodecFactory& factory)
{
    if (auto r = CheckPhase("AddIoServer"); !r)
        return std::unexpected(r.error());
    auto parsed = ParseEndpoint(endpoint);
    if (!parsed)
        return std::unexpected(parsed.error());
    if (parsed->ws)
        return Fail(Errc::Module, fmt::format("net: 端点 '{}' 是 ws:// / wss://（AddIoServer 只收字节流：tcp:// / tls:// / unix://）", endpoint));
    if (parsed->tls && !tls_server_)
        return Fail(Errc::Module, fmt::format("net: 端点 '{}' 是 tls://，但没有 TLS 服务端上下文", endpoint));
    const auto& ep = parsed->base;
    const auto tcp = TcpEndpoint::From(ep);
    const auto uds = tcp ? std::optional<UdsEndpoint>{} : UdsEndpoint::From(ep);
    if (!tcp && !uds)
        return Fail(Errc::Module, fmt::format("net: 监听端点 '{}' 不是 tcp / unix（AddIoServer 不收 udp://）", endpoint));
    StreamServerOptions so;
    so.connection = options_.connection;
    so.tcp = options_.tcp;
    so.tls = parsed->tls ? tls_server_ : nullptr;
    return MakeStreamServer(tcp, uds, handler, factory, std::move(so));
}

std::expected<INetClient*, Error> NetModule::AddIoClient(std::string_view endpoint,
                                                         IConnectionHandler& handler,
                                                         const IFrameCodecFactory& factory,
                                                         ReconnectPolicy policy)
{
    if (auto r = CheckPhase("AddIoClient"); !r)
        return std::unexpected(r.error());
    auto parsed = ParseEndpoint(endpoint);
    if (!parsed)
        return std::unexpected(parsed.error());
    if (parsed->ws)
        return Fail(Errc::Module, fmt::format("net: 端点 '{}' 是 ws:// / wss://（AddIoClient 只收字节流：tcp:// / tls:// / unix://）", endpoint));
    if (parsed->tls && !tls_client_)
        return Fail(Errc::Module, fmt::format("net: 端点 '{}' 是 tls://，但没有 TLS 客户端上下文", endpoint));
    const auto& ep = parsed->base;
    const auto tcp = TcpEndpoint::From(ep);
    const auto uds = tcp ? std::optional<UdsEndpoint>{} : UdsEndpoint::From(ep);
    if (!tcp && !uds)
        return Fail(Errc::Module, fmt::format("net: 对端端点 '{}' 不是 tcp / unix（AddIoClient 不收 udp://）", endpoint));
    StreamClientOptions co;
    co.connection = options_.connection;
    co.tcp = options_.tcp;
    co.tls = parsed->tls ? tls_client_ : nullptr;
    return MakeStreamClient(tcp, uds, handler, factory, std::move(co), policy);
}

std::expected<RUdpServer*, Error> NetModule::AddRUdpServer(const RUdpEndpoint& endpoint, IMainLoopHandler& handler)
{
    RUdpServerOptions o;
    o.connection = options_.connection;
    o.rudp = options_.rudp;
    return AddRUdpServer(endpoint, handler, std::move(o));
}

std::expected<RUdpServer*, Error> NetModule::AddRUdpServer(const RUdpEndpoint& endpoint,
                                                           IMainLoopHandler& handler,
                                                           RUdpServerOptions options)
{
    if (auto r = CheckPhase("AddRUdpServer"); !r)
        return std::unexpected(r.error());
    if (!options.rudp.Valid())
        return Fail(Errc::Module,
                    fmt::format("net: rudp 服务端 {} 的传输参数不合法（见 RUdpOptions::Valid）", endpoint.ToString()));
    IConnectionHandler& wrapped = executor_.Wrap(handler);
    auto server = std::make_unique<RUdpServer>(*group_, endpoint, std::move(options), wrapped);
    RUdpServer* raw = server.get();
    servers_.push_back(std::move(server));
    return raw;
}

std::expected<RUdpClient*, Error> NetModule::AddRUdpClient(const RUdpEndpoint& server, IMainLoopHandler& handler)
{
    RUdpClientOptions o;
    o.connection = options_.connection;
    o.rudp = options_.rudp;
    return AddRUdpClient(server, handler, std::move(o));
}

std::expected<RUdpClient*, Error> NetModule::AddRUdpClient(const RUdpEndpoint& server,
                                                           IMainLoopHandler& handler,
                                                           RUdpClientOptions options)
{
    if (auto r = CheckPhase("AddRUdpClient"); !r)
        return std::unexpected(r.error());
    if (!options.rudp.Valid())
        return Fail(Errc::Module,
                    fmt::format("net: rudp 客户端 {} 的传输参数不合法（见 RUdpOptions::Valid）", server.ToString()));
    IConnectionHandler& wrapped = executor_.Wrap(handler);
    auto client = std::make_unique<RUdpClient>(*group_, server, std::move(options), wrapped);
    RUdpClient* raw = client.get();
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

std::expected<void, Error> NetModule::Start()
{
    if (phase_ != Phase::Inited)
        return Fail(Errc::Phase, "net: Start 之前必须先 Init");
    if (const int e = group_->Start(); e != 0)
        return Fail(Errc::Module,
                    fmt::format("net: 启动 EventLoopGroup 失败（io_threads={} max_connections_per_loop={}）: {}",
                                options_.group.io_threads,
                                options_.group.max_connections_per_loop,
                                std::strerror(e)));
    for (auto& s : servers_)
    {
        if (const int e = s->Start(); e != 0)
        {
            const std::string msg = fmt::format("net: 监听 {} 失败: {}", s->endpoint().ToString(), std::strerror(e));
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
                fmt::format("net: 启动对 {} 的连接失败: {}", c->endpoint().ToString(), std::strerror(e));
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
