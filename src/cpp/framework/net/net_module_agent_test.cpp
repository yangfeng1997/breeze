// NetModule 的 Agent 接入：一个 Agent（AddAgentServer，路由在 IO 线程内）加两个 App（AddAgentClient），
// App 的业务回调在各自的主循环线程上；一个 App 走共享内存、一个因为配置关掉了共享内存而只用 UDS，对业务透明。
// 参数错误（端点、通道名、io_threads）、配置到选项的映射。无测试框架依赖：失败即 abort。

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "base/net/agent/envelope.h"
#include "base/net/agent/route_table.h"
#include "base/net/codec/length_prefix_codec.h"
#include "base/net/testing/net_test_util.h"
#include "framework/app/app.h"
#include "framework/net/net_module.h"
#include "gen/config/net_config.h"

using namespace breeze;
using namespace breeze::agent;
using namespace breeze::testing;

namespace
{

bool Has(const std::string& s, std::string_view needle) { return s.find(needle) != std::string::npos; }

std::string MakeEnvelope(EnvType type, NodeId src, NodeId dst, std::uint64_t seq, std::string_view body)
{
    EnvelopeHead h{};
    h.magic = kEnvelopeMagic;
    h.version = kEnvelopeVersion;
    h.type = static_cast<std::uint8_t>(type);
    h.body_len = static_cast<std::uint32_t>(body.size());
    h.src = src;
    h.dst = dst;
    h.route_type = static_cast<std::uint8_t>(RouteType::kDirect);
    h.remaining_ms = 5000;
    h.seq = seq;
    std::string out(reinterpret_cast<const char*>(&h), sizeof(h));
    out.append(body);
    return out;
}

// App 的业务处理器：记录消息与回调所在的线程；可以挂一个收到消息时的钩子（在主循环线程上）。
class Biz final : public IMainLoopHandler
{
public:
    void OnConnected(ConnectionHandle h, const PeerInfo&) override
    {
        Note();
        const std::lock_guard lk(mu_);
        connected_.push_back(h);
    }
    void OnMessage(ConnectionHandle h, SharedBuffer msg) override
    {
        Note();
        std::string s(reinterpret_cast<const char*>(msg.bytes().data()), msg.bytes().size());
        if (hook)
            hook(h, s);
        const std::lock_guard lk(mu_);
        messages_.push_back(std::move(s));
    }
    void OnClosed(ConnectionHandle, CloseReason, int) override
    {
        Note();
        const std::lock_guard lk(mu_);
        ++closed_;
    }
    std::size_t messages() const
    {
        const std::lock_guard lk(mu_);
        return messages_.size();
    }
    std::size_t connected() const
    {
        const std::lock_guard lk(mu_);
        return connected_.size();
    }
    std::string message(std::size_t i) const
    {
        const std::lock_guard lk(mu_);
        return messages_.at(i);
    }
    std::vector<std::thread::id> threads() const
    {
        const std::lock_guard lk(mu_);
        return threads_;
    }
    std::function<void(ConnectionHandle, const std::string&)> hook;

private:
    void Note()
    {
        const std::lock_guard lk(mu_);
        const auto id = std::this_thread::get_id();
        for (const auto& t : threads_)
            if (t == id)
                return;
        threads_.push_back(id);
    }
    mutable std::mutex mu_;
    std::vector<ConnectionHandle> connected_;
    std::vector<std::string> messages_;
    std::size_t closed_ = 0;
    std::vector<std::thread::id> threads_;
};

NetModuleOptions Options(std::uint32_t io_threads = 1)
{
    NetModuleOptions o;
    o.group.io_threads = io_threads;
    o.group.max_connections_per_loop = 256;
    o.shutdown_deadline = std::chrono::milliseconds(2000);
    return o;
}

// Agent 一侧的业务模块：Init 里 AddAgentServer。
class AgentModule final : public Module
{
public:
    AgentModule(App& a, std::string listen, RouteTable& table)
        : Module(a)
        , listen_(std::move(listen))
        , table_(table)
    {
    }
    std::string_view Name() const override { return "agent"; }
    std::vector<std::string_view> DependsOn() const override { return {"net"}; }
    Result<> Init() override
    {
        auto* net = app().GetModule<NetModule>("net");
        auto s = net->AddAgentServer(listen_, table_);
        if (!s)
            return std::unexpected(s.error());
        server = *s;
        return {};
    }
    Result<> Start() override
    {
        ready.store(true, std::memory_order_release);
        return {};
    }
    AgentServer* server = nullptr;
    std::atomic<bool> ready{false};

private:
    std::string listen_;
    RouteTable& table_;
};

// App 一侧的业务模块：Init 里 AddAgentClient（端点与是否启用共享内存取自 NetModuleOptions::agent）。
class AppModule final : public Module
{
public:
    AppModule(App& a, Biz& biz, std::string busid, const IFrameCodecFactory& f)
        : Module(a)
        , biz_(biz)
        , busid_(std::move(busid))
        , f_(f)
    {
    }
    std::string_view Name() const override { return "appmod"; }
    std::vector<std::string_view> DependsOn() const override { return {"net"}; }
    Result<> Init() override
    {
        net = app().GetModule<NetModule>("net");
        auto c = net->AddAgentClient(busid_, biz_, f_);
        if (!c)
            return std::unexpected(c.error());
        client = *c;
        return {};
    }
    Result<> Start() override { return {}; }
    NetModule* net = nullptr;
    AgentClient* client = nullptr;

private:
    Biz& biz_;
    std::string busid_;
    const IFrameCodecFactory& f_;
};

// 在独立线程里跑 App::Run（它就是"主循环线程"），Run 返回之后在同一个线程上做收尾（NetModule::Shutdown 要求在这个线程上调用）。
struct Runner
{
    explicit Runner(App& app, std::function<void()> epilogue = {})
        : thread(
              [this, &app, epilogue = std::move(epilogue)]
              {
                  tid.store(std::this_thread::get_id());
                  result = app.Run(stop.get_token());
                  if (epilogue)
                      epilogue();
                  done = true;
              })
    {
    }
    ~Runner() { Stop(); }
    void Stop()
    {
        stop.request_stop();
        if (thread.joinable())
            thread.join();
    }
    std::stop_source stop;
    std::atomic<std::thread::id> tid{};
    Result<> result;
    std::atomic<bool> done{false};
    std::thread thread;
};

// ---- 用例 ----

void TestAgentWithTwoApps()
{
    const LeakCheck leak;
    TempDir dir;
    const std::string agent_uri = dir.Unix("agent.sock").ToString();
    StaticRouteTable table;
    LengthPrefixCodecFactory frames(1 << 20);
    constexpr NodeId kApp1 = 1001, kApp2 = 1002;

    // Agent
    App agent_app("agent");
    NetModule* agent_net = nullptr;
    agent_app.RegisterModule(
        [&](App& a)
        {
            auto m = std::make_unique<NetModule>(a, Options());
            agent_net = m.get();
            return m;
        });
    AgentModule* am = nullptr;
    agent_app.RegisterModule(
        [&](App& a)
        {
            auto m = std::make_unique<AgentModule>(a, agent_uri, table);
            am = m.get();
            return m;
        });
    Runner agent_run(agent_app, [&] { agent_net->Shutdown(); });
    CHECK(WaitUntil([&] { return am != nullptr && am->ready.load(); }));

    // App1：默认配置（启用共享内存）。App2：配置关掉共享内存，只用 UDS。
    Biz biz1, biz2;
    NetModuleOptions o1 = Options();
    o1.agent.endpoint = agent_uri;
    o1.agent.shm_enabled = true;
    o1.agent.ring_capacity = 1 << 20;
    NetModuleOptions o2 = Options();
    o2.agent.endpoint = agent_uri;
    o2.agent.shm_enabled = false;
    App app1("app1"), app2("app2");
    NetModule *net1 = nullptr, *net2 = nullptr;
    app1.RegisterModule(
        [&](App& a)
        {
            auto m = std::make_unique<NetModule>(a, o1);
            net1 = m.get();
            return m;
        });
    app2.RegisterModule(
        [&](App& a)
        {
            auto m = std::make_unique<NetModule>(a, o2);
            net2 = m.get();
            return m;
        });
    AppModule* m1 = nullptr;
    AppModule* m2 = nullptr;
    app1.RegisterModule(
        [&](App& a)
        {
            auto m = std::make_unique<AppModule>(a, biz1, std::to_string(kApp1), frames);
            m1 = m.get();
            return m;
        });
    app2.RegisterModule(
        [&](App& a)
        {
            auto m = std::make_unique<AppModule>(a, biz2, std::to_string(kApp2), frames);
            m2 = m.get();
            return m;
        });
    Runner run1(app1, [&] { net1->Shutdown(); });
    Runner run2(app2, [&] { net2->Shutdown(); });
    CHECK(WaitUntil([&] { return m1 != nullptr && m1->client != nullptr && m2 != nullptr && m2->client != nullptr; }));
    // 对业务透明，但底下走的传输不同：App1 共享内存，App2 仅 UDS。
    CHECK(WaitUntil([&] { return m1->client->link() == AgentClient::Link::kShm; }));
    CHECK(WaitUntil([&] { return m2->client->link() == AgentClient::Link::kUds; }));
    CHECK(WaitUntil([&] { return biz1.connected() == 1 && biz2.connected() == 1; }));

    // App2 收到请求后在主循环线程上回一个 Response。
    biz2.hook = [&](ConnectionHandle h, const std::string& msg)
    {
        EnvelopeHead req{};
        CHECK(ParseEnvelope(std::span(reinterpret_cast<const std::byte*>(msg.data()), msg.size()), req) == EnvStatus::kOk);
        const std::string resp = MakeEnvelope(EnvType::kResponse, kApp2, req.src, req.seq, "pong:" + msg.substr(sizeof(req)));
        CHECK(h.Send(SharedBuffer::CopyFrom(std::span(reinterpret_cast<const std::byte*>(resp.data()), resp.size()))) ==
              SendResult::Ok);
    };
    for (std::uint64_t seq = 1; seq <= 20; ++seq)
    {
        const std::string req = MakeEnvelope(EnvType::kRequest, kApp1, kApp2, seq, "ping" + std::to_string(seq));
        CHECK(m1->client->Send(SharedBuffer::CopyFrom(std::span(reinterpret_cast<const std::byte*>(req.data()), req.size()))) ==
              SendResult::Ok); // 经 INetClient::Send：任意线程
    }
    CHECK(WaitUntil([&] { return biz2.messages() == 20 && biz1.messages() == 20; }));
    for (std::uint64_t i = 0; i < 20; ++i)
    {
        EnvelopeHead h{};
        const std::string r = biz1.message(i);
        CHECK(ParseEnvelope(std::span(reinterpret_cast<const std::byte*>(r.data()), r.size()), h) == EnvStatus::kOk);
        CHECK(h.type == static_cast<std::uint8_t>(EnvType::kResponse) && h.src == kApp2 && h.dst == kApp1 && h.seq == i + 1);
        CHECK(r.substr(sizeof(h)) == "pong:ping" + std::to_string(i + 1)); // 保序、内容原样
        CHECK(h.hops == 1 && h.remaining_ms > 4000 && h.remaining_ms <= 5000); // 回包是新消息，hops 从 0 起，经 Agent 一跳
    }
    // 业务回调都在各自 App 的主循环线程上（共享内存的连接也一样）。
    CHECK(biz1.threads().size() == 1 && biz1.threads()[0] == run1.tid.load());
    CHECK(biz2.threads().size() == 1 && biz2.threads()[0] == run2.tid.load());
    CHECK(run1.tid.load() != run2.tid.load() && run1.tid.load() != agent_run.tid.load());
    // Agent 里的统计：从共享内存收到 20 条（App1）、从仅 UDS 收到 20 条（App2）。
    // delivered_* 在写进目的地之后才加：业务收到消息时计数可能还没到，要等。
    CHECK(WaitUntil(
        [&]
        {
            const AgentRouterStats s = am->server->router().stats();
            return s.received_shm == 20 && s.received_uds == 20 && s.delivered_shm == 20 && s.delivered_uds == 20;
        }));
    const AgentRouterStats st = am->server->router().stats();
    CHECK(st.bad_envelope == 0 && st.bad_src == 0 && st.no_route == 0 && st.local_nodes == 2);
    CHECK(am->server->shm_stats().uds_channels == 1 && am->server->shm_stats().open_channels == 2);
    CHECK(m1->client->stats().tx_messages == 20 && m1->client->stats().rx_messages == 20);

    // 停机：先停 App；各自的 Run 线程在 Run 返回之后调 NetModule::Shutdown（由 main 负责，必须在主循环线程上）。
    run1.Stop();
    run2.Stop();
    agent_run.Stop();
    CHECK(run1.result.has_value() && run2.result.has_value() && agent_run.result.has_value());
}

// 配置里的 net.agent_* 映射到选项；关掉共享内存就是 kUdsOnly。
void TestConfigMapsToOptions()
{
    config::NetConfig c;
    NetModuleOptions o = NetModuleOptionsFrom(c);
    CHECK(o.agent.endpoint == "unix:///run/breeze/agent.sock" && o.agent.shm_enabled && o.agent.ring_capacity == 0); // 默认值
    c.agentEndpoint = "unix-abstract://my-agent";
    c.agentShmEnabled = false;
    c.agentShmRingMb = 4;
    o = NetModuleOptionsFrom(c);
    CHECK(o.agent.endpoint == "unix-abstract://my-agent" && !o.agent.shm_enabled && o.agent.ring_capacity == (4ULL << 20));
}

// 装配参数的错误：在模块 Init 里返回，App::Run 立刻带着原因返回。
class FailModule final : public Module
{
public:
    FailModule(App& a, std::function<Result<>(NetModule&)> f)
        : Module(a)
        , f_(std::move(f))
    {
    }
    std::string_view Name() const override { return "fail"; }
    std::vector<std::string_view> DependsOn() const override { return {"net"}; }
    Result<> Init() override { return f_(*app().GetModule<NetModule>("net")); }
    Result<> Start() override { return {}; }

private:
    std::function<Result<>(NetModule&)> f_;
};

std::string RunInitFailure(NetModuleOptions options, std::function<Result<>(NetModule&)> assemble)
{
    App app("net-test");
    app.RegisterModule([&](App& a) { return std::make_unique<NetModule>(a, options); });
    app.RegisterModule([&](App& a) { return std::make_unique<FailModule>(a, assemble); });
    std::stop_source ss;
    const auto r = app.Run(ss.get_token());
    CHECK(!r.has_value());
    return r.error().msg;
}

void TestAssemblyErrors()
{
    const LeakCheck leak;
    Biz biz;
    LengthPrefixCodecFactory frames;
    StaticRouteTable table;
    { // Agent 的端点必须是 UDS：fd 只能经 UDS 传递。
        const std::string e = RunInitFailure(Options(),
                                             [&](NetModule& n) -> Result<>
                                             {
                                                 agent::AgentClientOptions o;
                                                 o.busid = "x";
                                                 auto c = n.AddAgentClient("tcp://127.0.0.1:1", biz, std::move(o));
                                                 return c ? Result<>{} : std::unexpected(c.error());
                                             });
        CHECK(Has(e, "tcp://127.0.0.1:1") && Has(e, "UDS"));
    }
    { // 通道名不合法。
        const std::string e = RunInitFailure(Options(),
                                             [&](NetModule& n) -> Result<>
                                             {
                                                 auto c = n.AddAgentClient("bad name", biz, frames);
                                                 return c ? Result<>{} : std::unexpected(c.error());
                                             });
        CHECK(Has(e, "bad name"));
    }
    { // 路由器是单线程的：io_threads 不是 1 就拒绝。
        const std::string e = RunInitFailure(Options(/*io_threads=*/2),
                                             [&](NetModule& n) -> Result<>
                                             {
                                                 auto s = n.AddAgentServer("unix:///tmp/never-used.sock", table);
                                                 return s ? Result<>{} : std::unexpected(s.error());
                                             });
        CHECK(Has(e, "io_threads") && Has(e, "2"));
    }
    { // 监听端点不合法 / 不是 UDS。
        const std::string e = RunInitFailure(Options(),
                                             [&](NetModule& n) -> Result<>
                                             {
                                                 auto s = n.AddAgentServer("tcp://127.0.0.1:0", table);
                                                 return s ? Result<>{} : std::unexpected(s.error());
                                             });
        CHECK(Has(e, "tcp://127.0.0.1:0"));
    }
    { // AddIoServer 不收 udp://。
        const std::string e = RunInitFailure(Options(),
                                             [&](NetModule& n) -> Result<>
                                             {
                                                 AgentRouter router(table);
                                                 auto s = n.AddIoServer("udp://127.0.0.1:0", router, frames);
                                                 return s ? Result<>{} : std::unexpected(s.error());
                                             });
        CHECK(Has(e, "udp://127.0.0.1:0"));
    }
}

} // namespace

int main()
{
    TestAgentWithTwoApps();
    TestConfigMapsToOptions();
    TestAssemblyErrors();
    std::puts("net_module_agent_test: ok");
    return 0;
}
