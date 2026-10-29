// NetModule 挂进真实的 App：业务回调在 App 主循环线程上、Wake 的预算耗尽能续跑、NetClient 经模块装配、
// Start 失败时自清理且 Run 返回带 endpoint 与 strerror 的错误、非法 endpoint /
// 错误阶段的错误、配置驱动的选项、停机排空。 无测试框架依赖：失败即 abort。

#include "net_module.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "app.h"
#include "config_module.h"
#include "length_prefix_codec.h"
#include "net_test_util.h"

using namespace breeze;
using namespace breeze::testing;

namespace
{

bool Has(const std::string& s, std::string_view needle) { return s.find(needle) != std::string::npos; }

// 业务 handler：记录事件与回调线程，收到消息原样回显（经句柄，任意线程可用）。
class Biz final : public MainLoopHandler
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
        if (echo)
            (void)h.Send(msg);
        const std::lock_guard lk(mu_);
        messages_.push_back(Str(msg.bytes()));
    }
    void OnClosed(ConnectionHandle h, CloseReason r, int) override
    {
        Note();
        const std::lock_guard lk(mu_);
        closed_.emplace_back(h, r);
    }

    std::size_t connected() const
    {
        const std::lock_guard lk(mu_);
        return connected_.size();
    }
    std::size_t messages() const
    {
        const std::lock_guard lk(mu_);
        return messages_.size();
    }
    std::size_t closed() const
    {
        const std::lock_guard lk(mu_);
        return closed_.size();
    }
    std::vector<std::string> payloads() const
    {
        const std::lock_guard lk(mu_);
        return messages_;
    }
    std::vector<std::pair<ConnectionHandle, CloseReason>> closed_list() const
    {
        const std::lock_guard lk(mu_);
        return closed_;
    }
    ConnectionHandle first_connected() const
    {
        const std::lock_guard lk(mu_);
        return connected_.empty() ? ConnectionHandle{} : connected_.front();
    }
    // 回调发生过的线程集合（用线程 id 的字符串去重）。
    std::vector<std::thread::id> threads() const
    {
        const std::lock_guard lk(mu_);
        return threads_;
    }

    bool echo = true;

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
    std::vector<std::pair<ConnectionHandle, CloseReason>> closed_;
    std::vector<std::thread::id> threads_;
};

// 依赖 net 的"业务模块"：在 Init 里装配 server，Start 时（net 已 Start）发布实际绑定的端点。
class BizModule final : public Module
{
public:
    BizModule(App& app, Biz& biz, std::string listen)
        : Module(app)
        , biz_(biz)
        , listen_(std::move(listen))
    {
    }

    std::string_view Name() const override { return "biz"; }
    std::vector<std::string_view> DependsOn() const override { return {"net"}; }

    Result<> Init() override
    {
        net_ = app().GetModule<NetModule>("net");
        auto s = net_->AddServer(listen_, biz_, factory_);
        if (!s)
            return std::unexpected(s.error());
        server_ = *s;
        return {};
    }
    Result<> Start() override
    {
        bound_ = server_->endpoint();
        ready_.store(true, std::memory_order_release);
        return {};
    }

    bool ready() const { return ready_.load(std::memory_order_acquire); }
    const Endpoint& bound() const { return bound_; }
    NetModule* net() const { return net_; }

private:
    Biz& biz_;
    std::string listen_;
    LengthPrefixCodecFactory factory_{1 << 20};
    NetModule* net_ = nullptr;
    NetServer* server_ = nullptr;
    Endpoint bound_ = *Endpoint::Tcp("127.0.0.1", 1);
    std::atomic<bool> ready_{false};
};

NetModuleOptions SmallOptions(std::size_t wake_budget = 1024)
{
    NetModuleOptions o;
    o.group = {2, 256};
    o.wake_budget = wake_budget;
    o.shutdown_deadline = 2000ms;
    return o;
}

// 在独立线程里跑 App::Run（它就是"主循环线程"）。
struct Runner
{
    explicit Runner(App& app)
        : thread(
              [this, &app]
              {
                  tid.store(std::this_thread::get_id());
                  result = app.Run(stop.get_token());
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

// ---- 回调在主循环线程；Wake 预算耗尽能续跑 ----

void TestCallbacksOnMainLoopAndWakeBudgetContinues()
{
    const LeakCheck leak;
    Biz biz;
    App app("net-test");
    app.RegisterModule([](App& a) { return std::make_unique<NetModule>(a, SmallOptions(/*wake_budget=*/4)); });
    BizModule* bm = nullptr;
    app.RegisterModule(
        [&](App& a)
        {
            auto m = std::make_unique<BizModule>(a, biz, "tcp://127.0.0.1:0");
            bm = m.get();
            return m;
        });
    Runner run(app);
    CHECK(WaitUntil([&] { return bm != nullptr && bm->ready(); }));

    TestClient c(bm->bound());
    constexpr int kMsgs = 200; // 预算只有 4：Wake 返回 true、App 补唤醒才能全部处理完
    for (int i = 0; i < kMsgs; ++i)
        c.SendFrame("m" + std::to_string(i));
    for (int i = 0; i < kMsgs; ++i)
        CHECK(c.RecvFrame() == "m" + std::to_string(i));       // 业务经句柄回显，顺序不乱
    CHECK(WaitUntil([&] { return biz.messages() == kMsgs; })); // 回显先发、再记录：客户端收完时最后一条可能还没记上
    CHECK(biz.connected() == 1);

    // 所有业务回调都在 App::Run 所在的线程上
    const auto threads = biz.threads();
    CHECK(threads.size() == 1 && threads[0] == run.tid.load());
    CHECK(threads[0] != std::this_thread::get_id());
    CHECK(bm->net()->executor().GetStats().dispatched >= kMsgs + 1);
}

// ---- Start 之后不能再装配 ----

void TestAssemblyAfterStartIsPhaseError()
{
    const LeakCheck leak;
    Biz biz;
    LengthPrefixCodecFactory factory;
    App app("net-test");
    NetModule* net = nullptr;
    app.RegisterModule(
        [&](App& a)
        {
            auto m = std::make_unique<NetModule>(a, SmallOptions());
            net = m.get();
            return m;
        });
    BizModule* bm = nullptr;
    app.RegisterModule(
        [&](App& a)
        {
            auto m = std::make_unique<BizModule>(a, biz, "tcp://127.0.0.1:0");
            bm = m.get();
            return m;
        });
    Runner run(app);
    CHECK(WaitUntil([&] { return bm != nullptr && bm->ready(); }));
    const auto late_client = net->AddClient("tcp://127.0.0.1:1", biz, factory);
    CHECK(!late_client && late_client.error().code == Errc::Phase && Has(late_client.error().msg, "AddClient"));
    const auto late_server = net->AddServer("tcp://127.0.0.1:0", biz, factory);
    CHECK(!late_server && late_server.error().code == Errc::Phase && Has(late_server.error().msg, "AddServer"));
}

// client 通路：先起 server 的 App 拿到端点，再起第二个 App，用 AddClient 连过去。
void TestClientThroughModule()
{
    const LeakCheck leak;
    Biz server_biz;
    App server_app("net-server");
    server_app.RegisterModule([](App& a) { return std::make_unique<NetModule>(a, SmallOptions()); });
    BizModule* bm = nullptr;
    server_app.RegisterModule(
        [&](App& a)
        {
            auto m = std::make_unique<BizModule>(a, server_biz, "tcp://127.0.0.1:0");
            bm = m.get();
            return m;
        });
    Runner server_run(server_app);
    CHECK(WaitUntil([&] { return bm != nullptr && bm->ready(); }));
    const std::string target = bm->bound().ToString();

    Biz client_biz;
    client_biz.echo = false;
    LengthPrefixCodecFactory factory(1 << 20);
    NetClient* client = nullptr;
    class ClientModule final : public Module
    {
    public:
        ClientModule(App& a, Biz& biz, const FrameCodecFactory& f, std::string target, NetClient*& out)
            : Module(a)
            , biz_(biz)
            , f_(f)
            , target_(std::move(target))
            , out_(out)
        {
        }
        std::string_view Name() const override { return "cli"; }
        std::vector<std::string_view> DependsOn() const override { return {"net"}; }
        Result<> Init() override
        {
            auto c = app().GetModule<NetModule>("net")->AddClient(target_, biz_, f_);
            if (!c)
                return std::unexpected(c.error());
            out_ = *c;
            return {};
        }
        Result<> Start() override { return {}; }

    private:
        Biz& biz_;
        const FrameCodecFactory& f_;
        std::string target_;
        NetClient*& out_;
    };
    App client_app("net-client");
    client_app.RegisterModule([](App& a) { return std::make_unique<NetModule>(a, SmallOptions()); });
    client_app.RegisterModule([&](App& a)
                              { return std::make_unique<ClientModule>(a, client_biz, factory, target, client); });
    Runner client_run(client_app);
    CHECK(WaitUntil([&] { return client_biz.connected() == 1; }));
    CHECK(client != nullptr);

    CHECK(client->Send(Buf("ping")) == SendResult::Ok);
    CHECK(WaitUntil([&] { return client_biz.messages() == 1; })); // server 业务回显 → client 业务收到
    CHECK(client_biz.payloads()[0] == "ping");
    CHECK(client_biz.threads().size() == 1 && client_biz.threads()[0] == client_run.tid.load());
    CHECK(server_biz.threads().size() == 1 && server_biz.threads()[0] == server_run.tid.load());
}

// ---- 停机：Run 返回之后 main 调 Shutdown，业务看到每条连接的 OnClosed ----

void TestShutdownDrainsEveryClosed()
{
    const LeakCheck leak;
    Biz biz;
    App app("net-test");
    app.RegisterModule([](App& a) { return std::make_unique<NetModule>(a, SmallOptions()); });
    BizModule* bm = nullptr;
    app.RegisterModule(
        [&](App& a)
        {
            auto m = std::make_unique<BizModule>(a, biz, "tcp://127.0.0.1:0");
            bm = m.get();
            return m;
        });
    // 与生产一致：本线程（相当于 main）既跑 App::Run 又在它返回之后调 Shutdown——事件队列是
    // SPSC，消费者必须是同一个线程。 客户端的交互放到另一个线程，做完请求停止。
    std::stop_source ss;
    std::vector<std::unique_ptr<TestClient>> clients;
    std::size_t closed_before_shutdown = SIZE_MAX;
    std::thread driver(
        [&]
        {
            CHECK(WaitUntil([&] { return bm != nullptr && bm->ready(); }));
            for (int i = 0; i < 3; ++i)
                clients.push_back(std::make_unique<TestClient>(bm->bound()));
            for (auto& c : clients)
            {
                c->SendFrame("hi");
                CHECK(c->RecvFrame() == "hi");
            }
            CHECK(biz.connected() == 3);
            closed_before_shutdown = biz.closed();
            ss.request_stop(); // Run 返回：主循环不再 Drain
        });
    const auto r = app.Run(ss.get_token());
    driver.join();
    CHECK(r);
    CHECK(closed_before_shutdown == 0); // 连接还开着

    auto* net = app.GetModule<NetModule>("net");
    CHECK(net != nullptr);
    net->Shutdown(); // group 优雅关闭，再把事件 Drain 到空
    CHECK(biz.closed() == 3);
    for (const auto& [h, reason] : biz.closed_list())
        CHECK(reason == CloseReason::LocalClose);
    CHECK(!net->group().running());
    CHECK(!net->Wake()); // 已空
    for (auto& c : clients)
        CHECK(c->WaitClosed()); // 对端都得到了 EOF
    net->Shutdown();            // 重复调用是空操作
}

// ---- Start 失败：自清理，Run 返回带 endpoint 与 strerror 的错误 ----

void TestStartFailureRollsBackAndReportsEndpoint()
{
    const LeakCheck leak;
    // 占住一个端口
    Fd blocker;
    const Endpoint any = *Endpoint::Tcp("127.0.0.1", 0);
    CHECK(sock::Create(any, blocker) == 0 && sock::Bind(blocker.get(), any) == 0 &&
          sock::Listen(blocker.get(), 8) == 0);
    std::optional<Endpoint> taken;
    CHECK(sock::LocalEndpoint(blocker.get(), taken) == 0 && taken);

    Biz biz;
    App app("net-test");
    app.RegisterModule([](App& a) { return std::make_unique<NetModule>(a, SmallOptions()); });
    app.RegisterModule([&](App& a) { return std::make_unique<BizModule>(a, biz, taken->ToString()); });
    std::stop_source ss;
    const auto r = app.Run(ss.get_token()); // 直接在本线程跑：Start 失败时立即返回
    CHECK(!r);
    CHECK(r.error().code == Errc::Module);
    CHECK(Has(r.error().msg, taken->ToString()));
    CHECK(Has(r.error().msg, std::strerror(EADDRINUSE)));
    auto* net = app.GetModule<NetModule>("net");
    CHECK(net != nullptr && !net->group().running()); // 已经 Shutdown
    // 线程与 fd 没有泄漏由 LeakCheck 在作用域结束时检查（App 与模块随之析构）
}

// ---- 装配的错误 ----

void TestAssemblyErrors()
{
    const LeakCheck leak;
    Biz biz;
    LengthPrefixCodecFactory factory;
    App app("net-test");
    NetModule* net = nullptr;
    app.RegisterModule(
        [&](App& a)
        {
            auto m = std::make_unique<NetModule>(a, SmallOptions());
            net = m.get();
            return m;
        });
    CHECK(app.Build());
    // Init 之前：阶段错误
    const auto early = net->AddServer("tcp://127.0.0.1:0", biz, factory);
    CHECK(!early && early.error().code == Errc::Phase);
    CHECK(app.Init());
    // 非法 endpoint：错误里带着原字符串
    for (const char* bad : {"tcp://nope", "127.0.0.1:80", "unix://relative/path", ""})
    {
        const auto s = net->AddServer(bad, biz, factory);
        CHECK(!s && s.error().code == Errc::Module && Has(s.error().msg, std::string("'") + bad + "'"));
        const auto c = net->AddClient(bad, biz, factory);
        CHECK(!c && Has(c.error().msg, std::string("'") + bad + "'"));
    }
    // 合法的可以装
    CHECK(net->AddServer("tcp://127.0.0.1:0", biz, factory));
    CHECK(net->AddClient("unix:///tmp/breeze-net-module-test.sock", biz, factory));
}

// ---- 配置 ----

void TestOptionsFromConfig()
{
    // 单位换算与字段搬运
    config::NetConfig c;
    c.ioThreads = 7;
    c.maxConnectionsPerLoop = 1000;
    c.highWaterMarkBytes = 4096;
    c.idleTimeoutMs = 1500;
    c.keepaliveIdleS = 30;
    c.keepaliveIntervalS = 5;
    c.keepaliveCount = 2;
    c.tcpUserTimeoutMs = 20000;
    c.inboundMaxMessagesPerConn = 10;
    c.inboundMaxBytesPerConn = 2048;
    c.inboundGlobalBudgetMb = 3;
    c.wakeBudget = 77;
    c.shutdownDeadlineMs = 1234;
    const NetModuleOptions o = NetModuleOptionsFrom(c);
    CHECK(o.group.io_threads == 7 && o.group.max_connections_per_loop == 1000);
    CHECK(o.connection.high_water_mark == 4096 && o.connection.idle_timeout == 1500ms);
    CHECK(o.connection.keepalive_idle == 30s && o.connection.keepalive_interval == 5s &&
          o.connection.keepalive_count == 2);
    CHECK(o.connection.tcp_user_timeout == 20000ms);
    CHECK(o.inbound.max_messages_per_conn == 10 && o.inbound.max_bytes_per_conn == 2048);
    CHECK(o.inbound.global_budget_bytes == 3u * 1024 * 1024);
    CHECK(o.wake_budget == 77 && o.shutdown_deadline == 1234ms);
}

// NetModule(app) 在 Init 时从 ConfigModule 读 net 段；真实的 config/common.yaml 单独就能起，取到的是 net.proto
// 的生产默认值。
void TestModuleReadsNetSectionFromConfig()
{
    const LeakCheck leak;
    ::unsetenv("BREEZE_REDIS_PWD");
    ::unsetenv("BREEZE_MONGO_URI");
    const std::string path = std::string(BREEZE_CONFIG_DIR) + "/common.yaml";
    App app("net-test");
    app.RegisterModule([path](App& a) { return std::make_unique<ConfigModule>(a, path); });
    app.RegisterModule([](App& a) { return std::make_unique<NetModule>(a); });
    CHECK(app.Build());
    CHECK(app.Init());
    auto* net = app.GetModule<NetModule>("net");
    CHECK(net != nullptr);
    const NetModuleOptions& o = net->options();
    CHECK(o.group.io_threads == 4 && o.group.max_connections_per_loop == 65536);
    CHECK(o.connection.high_water_mark == 262144 && o.connection.idle_timeout == 0ms);
    CHECK(o.connection.keepalive_idle == 60s && o.connection.keepalive_interval == 10s &&
          o.connection.keepalive_count == 3);
    CHECK(o.inbound.max_messages_per_conn == 256 && o.inbound.max_bytes_per_conn == 65536);
    CHECK(o.inbound.global_budget_bytes == 256ull * 1024 * 1024);
    CHECK(o.wake_budget == 1024 && o.shutdown_deadline == 5000ms);
}

// 依赖：配置模式的 NetModule 依赖 config；显式选项的不依赖。
void TestDependsOn()
{
    App app("net-test");
    NetModule from_config(app);
    NetModule explicit_opts(app, SmallOptions());
    CHECK(from_config.DependsOn() == std::vector<std::string_view>{"config"});
    CHECK(explicit_opts.DependsOn().empty());
    CHECK(from_config.Name() == "net");
}

} // namespace

int main()
{
    TestCallbacksOnMainLoopAndWakeBudgetContinues();
    TestAssemblyAfterStartIsPhaseError();
    TestClientThroughModule();
    TestShutdownDrainsEveryClosed();
    TestStartFailureRollsBackAndReportsEndpoint();
    TestAssemblyErrors();
    TestOptionsFromConfig();
    TestModuleReadsNetSectionFromConfig();
    TestDependsOn();
    std::puts("net_module_test: ok");
    return 0;
}
