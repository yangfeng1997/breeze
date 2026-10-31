// NetModule 挂进真实的 App：业务回调在 App 主循环线程上、Wake 的预算耗尽能续跑、StreamClient 经模块装配、
// Start 失败时自清理且 Run 返回带 endpoint 与 strerror 的错误、非法 endpoint /
// 错误阶段的错误、配置驱动的选项、停机排空。 无测试框架依赖：失败即 abort。

#include "net_module.h"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
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
#include "rudp_test_util.h"

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
    BizModule(App& app, Biz& biz, std::string listen, std::optional<StreamServerOptions> options = std::nullopt)
        : Module(app)
        , biz_(biz)
        , listen_(std::move(listen))
        , options_(std::move(options))
    {
    }

    std::string_view Name() const override { return "biz"; }
    std::vector<std::string_view> DependsOn() const override { return {"net"}; }

    Result<> Init() override
    {
        net_ = app().GetModule<NetModule>("net");
        auto s = options_ ? net_->AddServer(listen_, biz_, factory_, std::move(*options_))
                          : net_->AddServer(listen_, biz_, factory_);
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
    NetServer* server() const { return server_; }

private:
    Biz& biz_;
    std::string listen_;
    std::optional<StreamServerOptions> options_; // 有值就走显式选项的 AddServer 重载
    LengthPrefixCodecFactory factory_{1 << 20};
    NetModule* net_ = nullptr;
    NetServer* server_ = nullptr;
    Endpoint bound_ = *Endpoint::Tcp("127.0.0.1", 1);
    std::atomic<bool> ready_{false};
};

// 依赖 net 的"客户端业务模块"：在 Init 里装配一个 client。
class ClientModule final : public Module
{
public:
    using Explicit = std::pair<StreamClientOptions, ReconnectPolicy>;

    ClientModule(App& a,
                 Biz& biz,
                 const FrameCodecFactory& f,
                 std::string target,
                 NetClient*& out,
                 std::optional<Explicit> options = std::nullopt)
        : Module(a)
        , biz_(biz)
        , f_(f)
        , target_(std::move(target))
        , out_(out)
        , options_(std::move(options))
    {
    }
    std::string_view Name() const override { return "cli"; }
    std::vector<std::string_view> DependsOn() const override { return {"net"}; }
    Result<> Init() override
    {
        auto* net = app().GetModule<NetModule>("net");
        auto c = options_ ? net->AddClient(target_, biz_, f_, std::move(options_->first), options_->second)
                          : net->AddClient(target_, biz_, f_);
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
    std::optional<Explicit> options_;
};

// 依赖 net 的 rudp 服务端业务模块：Init 里 AddRudpServer，Start 时发布实际绑定的端点。
class RudpServerModule final : public Module
{
public:
    RudpServerModule(App& app, Biz& biz, std::optional<RudpServerOptions> options = std::nullopt)
        : Module(app)
        , biz_(biz)
        , options_(std::move(options))
    {
    }
    std::string_view Name() const override { return "rudp-server"; }
    std::vector<std::string_view> DependsOn() const override { return {"net"}; }
    Result<> Init() override
    {
        auto* net = app().GetModule<NetModule>("net");
        auto s = options_ ? net->AddRudpServer(RudpLoopback(), biz_, std::move(*options_))
                          : net->AddRudpServer(RudpLoopback(), biz_);
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
    RudpServer* server() const { return server_; }

private:
    Biz& biz_;
    std::optional<RudpServerOptions> options_;
    RudpServer* server_ = nullptr;
    Endpoint bound_ = *Endpoint::Udp("127.0.0.1", 1);
    std::atomic<bool> ready_{false};
};

// 依赖 net 的 rudp 客户端业务模块：Init 里 AddRudpClient（不随模块启动，业务拿到票据后自己 Start）。
class RudpClientModule final : public Module
{
public:
    RudpClientModule(App& app,
                     Biz& biz,
                     RudpEndpoint server,
                     std::atomic<RudpClient*>& out,
                     std::optional<RudpClientOptions> options = std::nullopt)
        : Module(app)
        , biz_(biz)
        , server_(std::move(server))
        , out_(out)
        , options_(std::move(options))
    {
    }
    std::string_view Name() const override { return "rudp-client"; }
    std::vector<std::string_view> DependsOn() const override { return {"net"}; }
    Result<> Init() override
    {
        auto* net = app().GetModule<NetModule>("net");
        auto c = options_ ? net->AddRudpClient(server_, biz_, std::move(*options_)) : net->AddRudpClient(server_, biz_);
        if (!c)
            return std::unexpected(c.error());
        out_.store(*c, std::memory_order_release);
        return {};
    }
    Result<> Start() override { return {}; }

private:
    Biz& biz_;
    RudpEndpoint server_;
    std::atomic<RudpClient*>& out_;
    std::optional<RudpClientOptions> options_;
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

// 本进程里设了某个 TCP_KEEPIDLE 的 TCP socket（选项里取特殊值，别处不会碰巧相同）：验证选项真的传到了 socket 上。
int FindTcpSocketWithKeepIdle(int idle_s)
{
    for (int fd = 0; fd < 1024; ++fd)
    {
        int v = 0;
        socklen_t len = sizeof(v);
        if (::getsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &v, &len) == 0 && v == idle_s)
            return fd;
    }
    return -1;
}

int GetIntOpt(int fd, int level, int name)
{
    int v = -1;
    socklen_t len = sizeof(v);
    CHECK(::getsockopt(fd, level, name, &v, &len) == 0);
    return v;
}

void CheckTcpOptions(int fd, int idle_s, int interval_s, int count, int user_timeout_ms)
{
    CHECK(GetIntOpt(fd, SOL_SOCKET, SO_KEEPALIVE) == 1);
    CHECK(GetIntOpt(fd, IPPROTO_TCP, TCP_KEEPIDLE) == idle_s);
    CHECK(GetIntOpt(fd, IPPROTO_TCP, TCP_KEEPINTVL) == interval_s);
    CHECK(GetIntOpt(fd, IPPROTO_TCP, TCP_KEEPCNT) == count);
    CHECK(GetIntOpt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT) == user_timeout_ms);
}

std::string AbstractUri(const char* tag)
{
    return std::string("unix-abstract://breeze-net-module-test-") + tag + "-" + std::to_string(::getpid());
}

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
void TestClientThroughModule(const std::string& listen)
{
    const LeakCheck leak;
    Biz server_biz;
    App server_app("net-server");
    server_app.RegisterModule([](App& a) { return std::make_unique<NetModule>(a, SmallOptions()); });
    BizModule* bm = nullptr;
    server_app.RegisterModule(
        [&](App& a)
        {
            auto m = std::make_unique<BizModule>(a, server_biz, listen);
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

// ---- 选项经 NetModule 传到类型化的服务端 / 客户端上（TCP 与 UDS 各走一支） ----

// NetModuleOptions::tcp 是 AddServer / AddClient 不另给选项时的默认：服务端 accept 的与客户端发起的 TCP 连接都得到它。
void TestModuleDefaultTcpOptionsReachSockets()
{
    const LeakCheck leak;
    NetModuleOptions so = SmallOptions();
    so.tcp.keepalive_idle = 53s;
    so.tcp.keepalive_interval = 11s;
    so.tcp.keepalive_count = 6;
    so.tcp.tcp_user_timeout = 27000ms;
    Biz server_biz;
    App server_app("net-server");
    server_app.RegisterModule([&](App& a) { return std::make_unique<NetModule>(a, so); });
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

    NetModuleOptions co = SmallOptions();
    co.tcp.keepalive_idle = 57s;
    co.tcp.keepalive_interval = 13s;
    co.tcp.keepalive_count = 4;
    co.tcp.tcp_user_timeout = 29000ms;
    Biz client_biz;
    LengthPrefixCodecFactory factory(1 << 20);
    NetClient* client = nullptr;
    App client_app("net-client");
    client_app.RegisterModule([&](App& a) { return std::make_unique<NetModule>(a, co); });
    client_app.RegisterModule(
        [&](App& a) { return std::make_unique<ClientModule>(a, client_biz, factory, bm->bound().ToString(), client); });
    Runner client_run(client_app);
    CHECK(WaitUntil([&] { return client_biz.connected() == 1 && server_biz.connected() == 1; }));

    const int server_fd = FindTcpSocketWithKeepIdle(53);
    const int client_fd = FindTcpSocketWithKeepIdle(57);
    CHECK(server_fd >= 0 && client_fd >= 0 && server_fd != client_fd);
    CheckTcpOptions(server_fd, 53, 11, 6, 27000);
    CheckTcpOptions(client_fd, 57, 13, 4, 29000);
}

// NetModuleOptions::connection 是 AddServer / AddClient 不另给选项时的默认：服务端 accept 的与客户端发起的连接都得到它。
void TestModuleDefaultConnectionOptionsReach(const std::string& listen)
{
    const LeakCheck leak;
    {
        NetModuleOptions so = SmallOptions();
        so.connection.idle_timeout = 300ms;
        Biz biz;
        App app("net-server");
        app.RegisterModule([&](App& a) { return std::make_unique<NetModule>(a, so); });
        BizModule* bm = nullptr;
        app.RegisterModule(
            [&](App& a)
            {
                auto m = std::make_unique<BizModule>(a, biz, listen);
                bm = m.get();
                return m;
            });
        Runner run(app);
        CHECK(WaitUntil([&] { return bm != nullptr && bm->ready(); }));
        TestClient idle(bm->bound());
        CHECK(WaitUntil([&] { return biz.closed() == 1; }));
        CHECK(biz.closed_list()[0].second == CloseReason::IdleTimeout);
    }
    {
        Biz server_biz;
        App server_app("net-server");
        server_app.RegisterModule([](App& a) { return std::make_unique<NetModule>(a, SmallOptions()); });
        BizModule* bm = nullptr;
        server_app.RegisterModule(
            [&](App& a)
            {
                auto m = std::make_unique<BizModule>(a, server_biz, listen);
                bm = m.get();
                return m;
            });
        Runner server_run(server_app);
        CHECK(WaitUntil([&] { return bm != nullptr && bm->ready(); }));

        NetModuleOptions co = SmallOptions();
        co.connection.idle_timeout = 300ms;
        Biz client_biz;
        client_biz.echo = false;
        LengthPrefixCodecFactory factory(1 << 20);
        NetClient* client = nullptr;
        App client_app("net-client");
        client_app.RegisterModule([&](App& a) { return std::make_unique<NetModule>(a, co); });
        client_app.RegisterModule(
            [&](App& a)
            { return std::make_unique<ClientModule>(a, client_biz, factory, bm->bound().ToString(), client); });
        Runner client_run(client_app);
        CHECK(WaitUntil([&] { return client_biz.closed() >= 1; }));
        CHECK(client_biz.closed_list()[0].second == CloseReason::IdleTimeout);
    }
}

// 显式选项的 AddServer 重载：每个字段都要传到类型化的服务端上。accept_filter 与 idle_timeout 对两种传输都生效。
void TestExplicitServerOptionsReachTypedServers(const std::string& listen)
{
    const LeakCheck leak;
    Biz biz;
    App app("net-test");
    app.RegisterModule([](App& a) { return std::make_unique<NetModule>(a, SmallOptions()); });
    std::atomic<int> filter_calls{0};
    StreamServerOptions so;
    so.connection.idle_timeout = 300ms;
    so.accept_filter = [&](const PeerInfo&) noexcept { return filter_calls.fetch_add(1) != 0; }; // 第一条拒绝，之后放行
    BizModule* bm = nullptr;
    app.RegisterModule(
        [&](App& a)
        {
            auto m = std::make_unique<BizModule>(a, biz, listen, std::move(so));
            bm = m.get();
            return m;
        });
    Runner run(app);
    CHECK(WaitUntil([&] { return bm != nullptr && bm->ready(); }));

    TestClient first(bm->bound());
    CHECK(first.WaitClosed());
    CHECK(bm->net()->group().GetStats().rejected_filter == 1);
    CHECK(biz.connected() == 0);

    TestClient second(bm->bound());
    CHECK(WaitUntil([&] { return biz.connected() == 1; }));
    CHECK(WaitUntil([&] { return biz.closed() == 1; })); // 什么都不发：idle_timeout 传到了连接上
    CHECK(biz.closed_list()[0].second == CloseReason::IdleTimeout);
}

// 显式选项里的 tcp 只对 TCP 端点生效，经显式重载也要传到 socket 上。
void TestExplicitServerTcpOptionsReachSocket()
{
    const LeakCheck leak;
    Biz biz;
    App app("net-test");
    app.RegisterModule([](App& a) { return std::make_unique<NetModule>(a, SmallOptions()); });
    StreamServerOptions so;
    so.tcp.keepalive_idle = 59s;
    so.tcp.keepalive_interval = 12s;
    so.tcp.keepalive_count = 5;
    so.tcp.tcp_user_timeout = 28000ms;
    BizModule* bm = nullptr;
    app.RegisterModule(
        [&](App& a)
        {
            auto m = std::make_unique<BizModule>(a, biz, "tcp://127.0.0.1:0", std::move(so));
            bm = m.get();
            return m;
        });
    Runner run(app);
    CHECK(WaitUntil([&] { return bm != nullptr && bm->ready(); }));
    TestClient c(bm->bound());
    CHECK(WaitUntil([&] { return biz.connected() == 1; }));
    const int fd = FindTcpSocketWithKeepIdle(59);
    CHECK(fd >= 0);
    CheckTcpOptions(fd, 59, 12, 5, 28000);
}

// 显式选项的 AddClient 重载：on_connect_failed、connection、tcp 都要传到类型化的客户端上。
void TestExplicitClientOptionsReachTypedClients(bool tcp)
{
    const LeakCheck leak;
    // 一个"连不上"的对端：TCP 是先占一个端口再释放，UDS 是不存在的抽象名
    std::string dead;
    if (tcp)
    {
        UniqueFd fd;
        std::optional<Endpoint> ep;
        CHECK(sock::CreateStreamSocket(*Endpoint::Tcp("127.0.0.1", 0), fd) == 0);
        CHECK(sock::Bind(fd.get(), *Endpoint::Tcp("127.0.0.1", 0)) == 0);
        CHECK(sock::LocalEndpoint(fd.get(), ep) == 0 && ep.has_value());
        dead = ep->ToString();
    }
    else
        dead = AbstractUri("dead");

    std::atomic<int> err{0};
    std::atomic<int> retry{-1};
    StreamClientOptions co;
    co.on_connect_failed = [&](int e, bool will_retry) noexcept
    {
        retry = will_retry ? 1 : 0;
        err = e;
    };
    Biz biz;
    LengthPrefixCodecFactory factory(1 << 20);
    NetClient* client = nullptr;
    App app("net-client");
    app.RegisterModule([](App& a) { return std::make_unique<NetModule>(a, SmallOptions()); });
    app.RegisterModule(
        [&](App& a)
        {
            return std::make_unique<ClientModule>(
                a, biz, factory, dead, client, ClientModule::Explicit{std::move(co), ReconnectPolicy{}});
        });
    Runner run(app);
    CHECK(WaitUntil([&] { return err.load() != 0; }));
    CHECK(err == ECONNREFUSED && retry == 1); // 抽象名没人监听时内核也是 ECONNREFUSED（路径型才是 ENOENT）
}

// connect_timeout ≤ 0 是不合法选项：显式选项传到类型化的客户端上，Start 失败，App::Run 报错返回。
void TestExplicitClientConnectTimeoutReaches(const std::string& target)
{
    const LeakCheck leak;
    StreamClientOptions co;
    co.connect_timeout = 0ms;
    Biz biz;
    LengthPrefixCodecFactory factory(1 << 20);
    NetClient* client = nullptr;
    App app("net-client");
    app.RegisterModule([](App& a) { return std::make_unique<NetModule>(a, SmallOptions()); });
    app.RegisterModule(
        [&](App& a)
        {
            return std::make_unique<ClientModule>(
                a, biz, factory, target, client, ClientModule::Explicit{std::move(co), ReconnectPolicy{}});
        });
    std::stop_source ss;
    const auto r = app.Run(ss.get_token()); // 直接在本线程跑：Start 失败时立即返回
    CHECK(!r);
    CHECK(Has(r.error().msg, std::strerror(EINVAL)));
}

// 显式客户端选项里的 connection 与 tcp：idle_timeout 让客户端侧的连接以 IdleTimeout 关闭（TCP 与 UDS 都生效）；
// tcp 只对 TCP 生效，传到 socket 上。
void TestExplicitClientConnectionAndTcpOptionsReach(const std::string& listen)
{
    const bool is_tcp = listen.starts_with("tcp://");
    const LeakCheck leak;
    Biz server_biz;
    App server_app("net-server");
    server_app.RegisterModule([](App& a) { return std::make_unique<NetModule>(a, SmallOptions()); });
    BizModule* bm = nullptr;
    server_app.RegisterModule(
        [&](App& a)
        {
            auto m = std::make_unique<BizModule>(a, server_biz, listen);
            bm = m.get();
            return m;
        });
    Runner server_run(server_app);
    CHECK(WaitUntil([&] { return bm != nullptr && bm->ready(); }));

    StreamClientOptions co;
    co.tcp.keepalive_idle = 61s;
    co.tcp.keepalive_interval = 14s;
    co.tcp.keepalive_count = 3;
    co.tcp.tcp_user_timeout = 26000ms;
    co.connection.idle_timeout = 600ms;
    Biz client_biz;
    client_biz.echo = false;
    LengthPrefixCodecFactory factory(1 << 20);
    NetClient* client = nullptr;
    App client_app("net-client");
    client_app.RegisterModule([](App& a) { return std::make_unique<NetModule>(a, SmallOptions()); });
    client_app.RegisterModule(
        [&](App& a)
        {
            return std::make_unique<ClientModule>(a,
                                                  client_biz,
                                                  factory,
                                                  bm->bound().ToString(),
                                                  client,
                                                  ClientModule::Explicit{std::move(co), ReconnectPolicy{}});
        });
    Runner client_run(client_app);
    CHECK(WaitUntil([&] { return client_biz.connected() >= 1; }));
    if (is_tcp)
    {
        const int fd = FindTcpSocketWithKeepIdle(61);
        CHECK(fd >= 0);
        CheckTcpOptions(fd, 61, 14, 3, 26000);
    }
    // 什么都不发：客户端侧的连接被 idle_timeout 关闭
    CHECK(WaitUntil([&] { return client_biz.closed() >= 1; }));
    CHECK(client_biz.closed_list()[0].second == CloseReason::IdleTimeout);
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

// 停机等待期间事件继续被消费：对端在宽限期里还在发的消息，在 Shutdown 返回之前就交给了业务。
// 连接进入优雅关闭后暂停读不再生效，如果主线程只阻塞等着，这些数据只会堆在事件队列里；
// 这里对端要等业务"收到"它的消息才肯关——没有边等边消费的话，只能等到 shutdown_deadline 被强关。
void TestShutdownKeepsDrainingWhileWaiting()
{
    const LeakCheck leak;
    Biz biz;
    biz.echo = false;
    App app("net-test");
    NetModule* net = nullptr;
    app.RegisterModule(
        [&](App& a)
        {
            auto m = std::make_unique<NetModule>(a, SmallOptions()); // shutdown_deadline = 2s
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
    constexpr int kLate = 50;
    std::stop_source ss;
    std::thread driver(
        [&]
        {
            CHECK(WaitUntil([&] { return bm != nullptr && bm->ready(); }));
            TestClient c(bm->bound());
            c.SendFrame("hi");
            CHECK(WaitUntil([&] { return biz.messages() == 1; }));
            ss.request_stop();                                         // Run 返回，main 随即调 Shutdown
            CHECK(WaitUntil([&] { return !net->group().running(); })); // 停机开始：服务端的连接进入优雅关闭
            for (int i = 0; i < kLate; ++i)
                c.SendFrame("late" + std::to_string(i));
            // 业务要在 Shutdown 还在等的时候就收到这些消息，我们才关闭连接
            CHECK(WaitUntil([&] { return biz.messages() == 1 + kLate; }, 1500ms));
        });
    const auto r = app.Run(ss.get_token());
    CHECK(r);
    const auto t0 = std::chrono::steady_clock::now();
    net->Shutdown();
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    driver.join(); // 客户端随之析构、关闭
    CHECK(biz.messages() == 1 + kLate);
    CHECK(biz.closed() == 1);
    CHECK(elapsed < 1800ms); // 没有等到 shutdown_deadline（2s）被强关
}

// ---- Start 失败：自清理，Run 返回带 endpoint 与 strerror 的错误 ----

void TestStartFailureRollsBackAndReportsEndpoint()
{
    const LeakCheck leak;
    // 占住一个端口
    UniqueFd blocker;
    const Endpoint any = *Endpoint::Tcp("127.0.0.1", 0);
    CHECK(sock::CreateStreamSocket(any, blocker) == 0 && sock::Bind(blocker.get(), any) == 0 &&
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
    const auto early_rs = net->AddRudpServer(RudpLoopback(), biz);
    CHECK(!early_rs && early_rs.error().code == Errc::Phase);
    const auto early_rc = net->AddRudpClient(RudpLoopback(1), biz);
    CHECK(!early_rc && early_rc.error().code == Errc::Phase);
    CHECK(app.Init());
    // 非法 endpoint：错误里带着原字符串
    for (const char* bad : {"tcp://nope", "127.0.0.1:80", "unix://relative/path", ""})
    {
        const auto s = net->AddServer(bad, biz, factory);
        CHECK(!s && s.error().code == Errc::Module && Has(s.error().msg, std::string("'") + bad + "'"));
        const auto c = net->AddClient(bad, biz, factory);
        CHECK(!c && Has(c.error().msg, std::string("'") + bad + "'"));
    }
    // udp:// 的服务端是 rudp，装得上（factory 被忽略）；客户端要凭票据握手，不收字符串形式，错误里指向 AddRudpClient
    {
        const auto s = net->AddServer("udp://127.0.0.1:0", biz, factory);
        CHECK(s && dynamic_cast<RudpServer*>(*s) != nullptr);
        const auto c = net->AddClient("udp://127.0.0.1:9", biz, factory);
        CHECK(!c && c.error().code == Errc::Module && Has(c.error().msg, "udp") && Has(c.error().msg, "AddRudpClient"));
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
    CHECK(o.tcp.keepalive_idle == 30s && o.tcp.keepalive_interval == 5s && o.tcp.keepalive_count == 2);
    CHECK(o.tcp.tcp_user_timeout == 20000ms);
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
    CHECK(o.tcp.keepalive_idle == 60s && o.tcp.keepalive_interval == 10s && o.tcp.keepalive_count == 3);
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

// ---- rudp ----

std::optional<rudp::Packet> ParsePacket(const std::optional<Bytes>& dg)
{
    if (!dg)
        return std::nullopt;
    return rudp::Parse(*dg);
}

// rudp 经模块：服务端随模块启动，客户端不随模块启动（业务拿到票据后才 Start）；
// 双方的业务回调都在各自 App 的主循环线程。
void TestRudpThroughModule()
{
    const LeakCheck leak;
    Biz server_biz;
    App server_app("net-server");
    server_app.RegisterModule([](App& a) { return std::make_unique<NetModule>(a, SmallOptions()); });
    RudpServerModule* sm = nullptr;
    server_app.RegisterModule(
        [&](App& a)
        {
            auto m = std::make_unique<RudpServerModule>(a, server_biz);
            sm = m.get();
            return m;
        });
    Runner server_run(server_app);
    CHECK(WaitUntil([&] { return sm != nullptr && sm->ready(); }));

    Biz client_biz;
    client_biz.echo = false;
    std::atomic<RudpClient*> client{nullptr};
    App client_app("net-client");
    client_app.RegisterModule([](App& a) { return std::make_unique<NetModule>(a, SmallOptions()); });
    client_app.RegisterModule(
        [&](App& a)
        { return std::make_unique<RudpClientModule>(a, client_biz, *RudpEndpoint::From(sm->bound()), client); });
    Runner client_run(client_app);
    CHECK(WaitUntil([&] { return client.load() != nullptr; }));

    std::this_thread::sleep_for(300ms);
    CHECK(client_biz.connected() == 0 && server_biz.connected() == 0); // 客户端不随模块启动：没有票据就不握手

    const auto ticket = sm->server()->IssueTicket(30s);
    CHECK(ticket.has_value());
    client.load()->SetTicket(*ticket);
    CHECK(client.load()->Start() == 0);
    CHECK(WaitUntil([&] { return client_biz.connected() == 1 && server_biz.connected() == 1; }));

    CHECK(client.load()->Send(Buf("ping")) == SendResult::Ok);
    CHECK(WaitUntil([&] { return client_biz.messages() == 1; })); // 服务端业务回显 → 客户端业务收到
    CHECK(client_biz.payloads()[0] == "ping");
    CHECK(client_biz.threads().size() == 1 && client_biz.threads()[0] == client_run.tid.load());
    CHECK(server_biz.threads().size() == 1 && server_biz.threads()[0] == server_run.tid.load());
}

// 模块的默认选项传到 rudp 服务端：NetModuleOptions::connection 的 idle_timeout 让不出声的会话以 IdleTimeout 关闭。
void TestRudpModuleDefaultsReachServer()
{
    const LeakCheck leak;
    NetModuleOptions o = SmallOptions();
    o.connection.idle_timeout = 600ms;
    Biz biz;
    App app("net-server");
    app.RegisterModule([o](App& a) { return std::make_unique<NetModule>(a, o); });
    RudpServerModule* sm = nullptr;
    app.RegisterModule(
        [&](App& a)
        {
            auto m = std::make_unique<RudpServerModule>(a, biz);
            sm = m.get();
            return m;
        });
    Runner run(app);
    CHECK(WaitUntil([&] { return sm != nullptr && sm->ready(); }));

    RawUdp raw;
    const auto t = sm->server()->IssueTicket(30s);
    CHECK(t.has_value());
    raw.SendTo(sm->bound(), rudp::EncodeHello(t->conv, t->token).view());
    const auto accept = ParsePacket(raw.Recv(2s));
    CHECK(accept && accept->type == rudp::PacketType::Accept);
    CHECK(WaitUntil([&] { return biz.connected() == 1; }));
    CHECK(WaitUntil([&] { return biz.closed() == 1; }, 8s)); // 不发心跳：被空闲检测关掉
    CHECK(biz.closed_list()[0].second == CloseReason::IdleTimeout);
}

// 字符串形式的 udp:// 服务端：显式选项里的 accept_filter 也生效（拒绝的收到 Reject(Filtered)）。
void TestUdpAddServerStringPassesFilter()
{
    const LeakCheck leak;
    Biz biz;
    StreamServerOptions so;
    so.accept_filter = [](const PeerInfo&) noexcept { return false; };
    App app("net-server");
    app.RegisterModule([](App& a) { return std::make_unique<NetModule>(a, SmallOptions()); });
    BizModule* bm = nullptr;
    app.RegisterModule(
        [&](App& a)
        {
            auto m = std::make_unique<BizModule>(a, biz, "udp://127.0.0.1:0", std::move(so));
            bm = m.get();
            return m;
        });
    Runner run(app);
    CHECK(WaitUntil([&] { return bm != nullptr && bm->ready(); }));
    auto* rs = dynamic_cast<RudpServer*>(bm->server());
    CHECK(rs != nullptr);
    CHECK(bm->bound().kind() == Endpoint::Kind::Udp && bm->bound().port() != 0);

    RawUdp raw;
    const auto t = rs->IssueTicket(30s);
    CHECK(t.has_value());
    raw.SendTo(bm->bound(), rudp::EncodeHello(t->conv, t->token).view());
    const auto reply = ParsePacket(raw.Recv(2s));
    CHECK(reply && reply->type == rudp::PacketType::Reject &&
          reply->reason == static_cast<std::uint8_t>(rudp::RejectReason::Filtered));
    CHECK(biz.connected() == 0);
}

// rudp 的装配：模块默认的 rudp 选项不合法时，不给选项的重载（用模块默认）报错，显式给合法选项的可以；
// 字符串形式的 udp:// 走模块默认，同样报错。
void TestRudpAssembly()
{
    const LeakCheck leak;
    Biz biz;
    LengthPrefixCodecFactory factory;
    NetModuleOptions bad = SmallOptions();
    bad.rudp.mtu = 100; // 低于下限
    App app("net-test");
    NetModule* net = nullptr;
    app.RegisterModule(
        [&](App& a)
        {
            auto m = std::make_unique<NetModule>(a, bad);
            net = m.get();
            return m;
        });
    CHECK(app.Build());
    CHECK(app.Init());

    const auto s = net->AddRudpServer(RudpLoopback(), biz);
    CHECK(!s && s.error().code == Errc::Module && Has(s.error().msg, "RudpOptions"));
    const auto c = net->AddRudpClient(RudpLoopback(1), biz);
    CHECK(!c && c.error().code == Errc::Module && Has(c.error().msg, "RudpOptions"));
    const auto str = net->AddServer("udp://127.0.0.1:0", biz, factory);
    CHECK(!str && str.error().code == Errc::Module && Has(str.error().msg, "RudpOptions"));

    RudpServerOptions inv;
    inv.rudp.recv_window = 10; // 低于 128
    CHECK(!net->AddRudpServer(RudpLoopback(), biz, std::move(inv)));
    RudpClientOptions invc;
    invc.rudp.dead_link = 1;
    CHECK(!net->AddRudpClient(RudpLoopback(1), biz, std::move(invc)));

    CHECK(net->AddRudpServer(RudpLoopback(), biz, RudpServerOptions{}));
    CHECK(net->AddRudpClient(RudpLoopback(1), biz, RudpClientOptions{}));
}

} // namespace

int main()
{
    TestCallbacksOnMainLoopAndWakeBudgetContinues();
    TestAssemblyAfterStartIsPhaseError();
    TestClientThroughModule("tcp://127.0.0.1:0");
    TestClientThroughModule(AbstractUri("echo"));
    TestModuleDefaultTcpOptionsReachSockets();
    TestExplicitServerOptionsReachTypedServers("tcp://127.0.0.1:0");
    TestExplicitServerOptionsReachTypedServers(AbstractUri("opts"));
    TestExplicitServerTcpOptionsReachSocket();
    TestExplicitClientOptionsReachTypedClients(/*tcp=*/true);
    TestExplicitClientOptionsReachTypedClients(/*tcp=*/false);
    TestExplicitClientConnectTimeoutReaches("tcp://127.0.0.1:1");
    TestExplicitClientConnectTimeoutReaches(AbstractUri("timeout"));
    TestExplicitClientConnectionAndTcpOptionsReach("tcp://127.0.0.1:0");
    TestExplicitClientConnectionAndTcpOptionsReach(AbstractUri("cliopts"));
    TestModuleDefaultConnectionOptionsReach("tcp://127.0.0.1:0");
    TestModuleDefaultConnectionOptionsReach(AbstractUri("defconn"));
    TestShutdownDrainsEveryClosed();
    TestShutdownKeepsDrainingWhileWaiting();
    TestStartFailureRollsBackAndReportsEndpoint();
    TestAssemblyErrors();
    TestRudpThroughModule();
    TestRudpModuleDefaultsReachServer();
    TestUdpAddServerStringPassesFilter();
    TestRudpAssembly();
    TestOptionsFromConfig();
    TestModuleReadsNetSectionFromConfig();
    TestDependsOn();
    std::puts("net_module_test: ok");
    return 0;
}
