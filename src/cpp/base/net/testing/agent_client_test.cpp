// App 一侧的 AgentClient（设计 §6.1 / §6.2 / §6.5）：握手协商出来的两种数据通道、退化与失败、
// Agent 掉线后的 Reattach（共享内存模式连接不关）与仅 UDS 的重连、App 热更重启的恢复、暂停读、重试与退避、并发发送、生命周期。
// Agent 一侧用 shm_channel_test_util.h 里的回显 Env（共享内存通道回显、仅 UDS 的连接也回显）。
//
// 无测试框架依赖：失败即 abort，并发用例带超时。

#include "base/net/agent/agent_client.h"
#include "base/net/agent/agent_server.h"
#include "base/net/agent/route_table.h"
#include "base/net/testing/shm_channel_test_util.h"

using namespace shmtest;
using namespace breeze::agent;

namespace
{

ShmServerOptions Ret(std::chrono::milliseconds retain = 30s)
{
    ShmServerOptions o;
    o.retain_default = retain;
    return o;
}

AgentClientOptions Opts(std::string busid, Transport tr = Transport::kShmPreferred)
{
    AgentClientOptions o;
    o.busid = std::move(busid);
    o.transport = tr;
    o.tx_capacity = shm::kMinRingCapacity; // 测试用小环
    o.rx_capacity = shm::kMinRingCapacity;
    o.reconnect = ReconnectPolicy{20ms, 200ms, 2.0, 0.0};
    o.handshake_timeout = 3s;
    return o;
}

// 一个 App：独立的 group + 记录事件的处理器 + AgentClient。声明顺序就是销毁顺序的反面：处理器要比 group 活得久。
struct AppEnv
{
    EventRecorder rec;
    LengthPrefixCodecFactory codec;
    std::mutex mu;
    std::vector<std::pair<int, bool>> failures; // on_connect_failed 收到的 (errno, will_retry)
    EventLoopGroup group;
    std::unique_ptr<AgentClient> client;

    AppEnv(const Endpoint& ep, AgentClientOptions o)
        : group(EventLoopGroupOptions{1, 256})
    {
        CHECK(group.Start() == 0);
        o.on_connect_failed = [this](int e, bool retry) noexcept
        {
            const std::lock_guard lk(mu);
            failures.emplace_back(e, retry);
        };
        client = std::make_unique<AgentClient>(group, ep, std::move(o), rec);
    }
    std::vector<std::pair<int, bool>> Failures()
    {
        const std::lock_guard lk(mu);
        return failures;
    }
    bool WaitLink(AgentClient::Link want, std::chrono::milliseconds timeout = 5s)
    {
        return WaitUntil([&] { return client->link() == want; }, timeout);
    }
    bool SendStr(std::string_view s) { return client->Send(Buf(s)) == SendResult::Ok; }
    // 等到收满 n 条，并核对它们依次是 prefix + i。
    void ExpectEchoes(const std::string& prefix, std::size_t first, std::size_t count)
    {
        CHECK(rec.WaitMessages(first + count));
        const auto p = rec.payloads();
        for (std::size_t i = 0; i < count; ++i)
            CHECK(p[first + i] == prefix + std::to_string(first + i));
    }
};

using Link = AgentClient::Link;

// 发 count 条 "<prefix><序号>"。
void SendMany(AppEnv& app, const std::string& prefix, std::size_t first, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i)
        CHECK(app.SendStr(prefix + std::to_string(first + i)));
}

// ---- 用例 ----

void TestEchoOnBothTransports()
{
    LeakCheck leak;
    struct Case
    {
        Transport tr;
        Link want;
    };
    for (const Case c : {Case{Transport::kShmRequired, Link::kShm},
                         Case{Transport::kShmPreferred, Link::kShm},
                         Case{Transport::kUdsOnly, Link::kUds}})
    {
        Env env(Ret(), true);
        EchoOnUds(env);
        AppEnv app(env.ep, Opts("c1", c.tr));
        CHECK(app.client->Start() == 0);
        CHECK(app.WaitLink(c.want));
        app.rec.WaitConnected(0);
        CHECK(app.client->handle().valid() && !app.client->agent_down());
        SendMany(app, "m", 0, 200);
        app.ExpectEchoes("m", 0, 200); // 保序、一条不少
        const AgentClientStats st = app.client->stats();
        CHECK(st.attaches == 1 && st.attach_failures == 0 && st.reattaches == 0);
        if (c.want == Link::kShm)
            CHECK(st.tx_messages == 200 && st.rx_messages == 200 && st.tx_full_drops == 0);
        else
            CHECK(st.tx_messages == 0); // 没走共享内存

        // Stop：OnClosed(LocalClose) 恰一次，之后发送得到 Closed；共享内存模式 Agent 收到 Bye，不保留。
        app.client->Stop();
        const auto closed = app.rec.WaitClosed(0);
        CHECK(closed.reason == CloseReason::LocalClose);
        CHECK(app.WaitLink(Link::kNone));
        CHECK(app.client->Send(Buf("x")) == SendResult::Closed);
        if (c.want == Link::kShm)
        {
            CHECK(WaitUntil([&] { return env.handler.closed_reason("c1") == ShmCloseReason::PeerBye; }, 2s));
            CHECK(env.server->stats().detached_total == 0);
        }
        else
        {
            CHECK(WaitUntil([&] { return env.server->stats().uds_channels == 0; }));
        }
        app.rec.CheckInvariants();
        CHECK(app.rec.closed_count() == 1 && app.rec.connected_count() == 1);
    }
}

void TestNegotiationOutcomes()
{
    LeakCheck leak;
    { // Agent 不提供共享内存：优先共享内存的退化成仅 UDS；必须共享内存的拿到不可重试的 ENOTSUP。
        ShmServerOptions o = Ret();
        o.allow_shm = false;
        Env env(o, true);
        EchoOnUds(env);
        AppEnv pref(env.ep, Opts("p1", Transport::kShmPreferred));
        CHECK(pref.client->Start() == 0);
        CHECK(pref.WaitLink(Link::kUds));
        CHECK(pref.SendStr("hello") && pref.rec.WaitMessages(1));
        CHECK(env.server->stats().downgraded == 1);

        AppEnv req(env.ep, Opts("p2", Transport::kShmRequired));
        CHECK(req.client->Start() == 0);
        CHECK(WaitUntil([&] { return !req.Failures().empty(); }));
        std::this_thread::sleep_for(300ms); // 不可重试：之后不会再试
        const auto f = req.Failures();
        CHECK(f.size() == 1 && f[0].first == ENOTSUP && !f[0].second);
        CHECK(req.client->link() == Link::kNone);
    }
    { // 共享内存的总量配额满了：优先的退化，必须的得到可重试的 ENOSPC（配额也许之后会腾出来）。
        ShmServerOptions o = Ret();
        o.max_total_bytes = 3ULL << 20;
        Env env(o, true);
        EchoOnUds(env);
        AppEnv holder(env.ep, Opts("q1", Transport::kShmRequired));
        CHECK(holder.client->Start() == 0);
        CHECK(holder.WaitLink(Link::kShm));
        AppEnv pref(env.ep, Opts("q2", Transport::kShmPreferred));
        CHECK(pref.client->Start() == 0);
        CHECK(pref.WaitLink(Link::kUds));
        AppEnv req(env.ep, Opts("q3", Transport::kShmRequired));
        CHECK(req.client->Start() == 0);
        CHECK(WaitUntil([&] { return req.Failures().size() >= 3; })); // 退避重试在继续
        for (const auto& [e, retry] : req.Failures())
            CHECK(e == ENOSPC && retry);
        // 配额腾出来：下一次重试就成功。
        holder.client->Stop();
        CHECK(req.WaitLink(Link::kShm));
    }
    { // Agent 没有提供仅 UDS 的数据通道：要仅 UDS 的得到不可重试的 ENOTSUP。
        Env env(Ret(), false);
        AppEnv app(env.ep, Opts("n1", Transport::kUdsOnly));
        CHECK(app.client->Start() == 0);
        CHECK(WaitUntil([&] { return !app.Failures().empty(); }));
        const auto f = app.Failures();
        CHECK(f[0].first == ENOTSUP && !f[0].second);
    }
    { // 准入被拒（身份 / 策略）：EACCES 不可重试。
        Env env(Ret(), true);
        env.handler.admit_deny.store(EACCES);
        AppEnv app(env.ep, Opts("denied", Transport::kShmRequired));
        CHECK(app.client->Start() == 0);
        CHECK(WaitUntil([&] { return !app.Failures().empty(); }));
        std::this_thread::sleep_for(300ms);
        CHECK(app.Failures().size() == 1 && app.Failures()[0].first == EACCES && !app.Failures()[0].second);
    }
    { // 配置不合法（容量不是 2 的幂）：Agent 回 EINVAL，不可重试，回到 Idle，之后不再试；再 Start 才再试一次。
        Env env(Ret(), true);
        AgentClientOptions o = Opts("bad", Transport::kShmRequired);
        o.tx_capacity = 3 * shm::kMinRingCapacity;
        AppEnv app(env.ep, std::move(o));
        CHECK(app.client->Start() == 0);
        CHECK(WaitUntil([&] { return !app.Failures().empty(); }));
        std::this_thread::sleep_for(400ms);
        CHECK(app.Failures().size() == 1 && app.Failures()[0].first == EINVAL && !app.Failures()[0].second);
        CHECK(app.client->Start() == 0);
        CHECK(WaitUntil([&] { return app.Failures().size() == 2; }));
    }
    { // 参数错误同步返回，不去连。
        Env env(Ret(), true);
        AppEnv a(env.ep, Opts("bad name"));
        CHECK(a.client->Start() == EINVAL);
        AgentClientOptions bt = Opts("bt");
        bt.transport = static_cast<Transport>(9);
        AppEnv b(env.ep, std::move(bt));
        CHECK(b.client->Start() == EINVAL);
        AgentClientOptions br = Opts("br");
        br.reconnect.initial_delay = 0ms;
        AppEnv c(env.ep, std::move(br));
        CHECK(c.client->Start() == EINVAL);
        EventLoopGroup g(EventLoopGroupOptions{1, 16});
        CHECK(g.Start() == 0);
        EventRecorder rec;
        AgentClient tcp(g, *Endpoint::Tcp("127.0.0.1", 1), Opts("tcp"), rec);
        CHECK(tcp.Start() == EPROTONOSUPPORT);
        CHECK(env.server->stats().opened == 0 && env.server->stats().rejected == 0);
    }
}

void TestAgentRestartKeepsShmConnection()
{
    LeakCheck leak;
    TempDir dir;
    const Endpoint ep = dir.Unix("agent.sock");
    auto agent = std::make_unique<Env>(Ret(), ep, true);
    EchoOnUds(*agent);
    AppEnv app(ep, Opts("app-r", Transport::kShmRequired));
    CHECK(app.client->Start() == 0);
    CHECK(app.WaitLink(Link::kShm));
    SendMany(app, "a", 0, 5);
    app.ExpectEchoes("a", 0, 5);

    for (int cycle = 1; cycle <= 2; ++cycle)
    {
        agent.reset(); // Agent 没了：通道销毁、UDS 关闭
        CHECK(WaitUntil([&] { return app.client->agent_down(); }));
        // **连接没关**：业务感觉不到，Send 照常写 Tx 环。
        CHECK(app.rec.closed_count() == 0 && app.client->link() == Link::kShm && app.client->handle().valid());
        const std::size_t base = app.rec.message_count();
        SendMany(app, "b" + std::to_string(cycle) + "-", base, 10);
        std::this_thread::sleep_for(100ms); // Agent 不在：没有回显
        CHECK(app.rec.message_count() == base);

        agent = std::make_unique<Env>(Ret(), ep, true); // 新 Agent
        EchoOnUds(*agent);
        CHECK(WaitUntil([&] { return !app.client->agent_down(); }, 10s));
        app.ExpectEchoes("b" + std::to_string(cycle) + "-", base, 10); // Agent 不在期间写的，新 Agent 接着处理
        CHECK(WaitUntil([&] { return agent->handler.reattached_opens() == 1; }));
        SendMany(app, "c" + std::to_string(cycle) + "-", base + 10, 5); // 之后一切照常
        app.ExpectEchoes("c" + std::to_string(cycle) + "-", base + 10, 5);
    }
    const AgentClientStats st = app.client->stats();
    CHECK(st.attaches == 1 && st.reattaches == 2 && st.agent_down_total == 2);
    CHECK(app.rec.connected_count() == 1 && app.rec.closed_count() == 0); // 全程是同一条连接
}

void TestAgentRestartUdsOnlyReconnects()
{
    LeakCheck leak;
    TempDir dir;
    const Endpoint ep = dir.Unix("agent.sock");
    auto agent = std::make_unique<Env>(Ret(), ep, true);
    EchoOnUds(*agent);
    AppEnv app(ep, Opts("app-u", Transport::kUdsOnly));
    CHECK(app.client->Start() == 0);
    CHECK(app.WaitLink(Link::kUds));
    SendMany(app, "a", 0, 5);
    app.ExpectEchoes("a", 0, 5);

    agent.reset(); // 仅 UDS：连接就是 UDS，断了就是关闭（没有队列可保留）
    const auto closed = app.rec.WaitClosed(0);
    CHECK(closed.reason == CloseReason::PeerClosed);
    CHECK(app.WaitLink(Link::kNone));
    agent = std::make_unique<Env>(Ret(), ep, true);
    EchoOnUds(*agent);
    CHECK(app.WaitLink(Link::kUds, 10s)); // 退避之后自动重新握手
    const std::size_t base = app.rec.message_count();
    SendMany(app, "b", base, 5);
    app.ExpectEchoes("b", base, 5);
    CHECK(app.rec.connected_count() == 2 && app.rec.closed_count() == 1);
    CHECK(app.client->stats().attaches == 2);
    app.rec.CheckInvariants();
}

void TestAppRestartResumesBacklog()
{
    LeakCheck leak;
    Env env(Ret(), true);
    EchoOnUds(env);
    {
        AppEnv a(env.ep, Opts("app-hot", Transport::kShmRequired));
        CHECK(a.client->Start() == 0);
        CHECK(a.WaitLink(Link::kShm));
        SendMany(a, "x", 0, 3);
        a.ExpectEchoes("x", 0, 3);
        // 热更重启：要 10 秒保留；之后的析构当作"重启"，不发 Bye。
        a.client->PrepareRestart(10s);
        std::this_thread::sleep_for(100ms); // 让控制消息先到
    }
    CHECK(WaitUntil([&] { return env.server->stats().detached_channels == 1; }));
    bool seen = false;
    for (const auto& e : env.handler.events())
        seen |= e.kind == EchoHandler::Event::Kind::Detached && e.retain == 10s;
    CHECK(seen);
    // 保留期里 Agent 往这个 App 的 Rx 写了 3 条。
    for (std::uint64_t i = 0; i < 3; ++i)
        CHECK(env.AgentPush("app-hot", 100 + i, LenOf(100 + i)));

    // 新进程（同一个 busid）连上：恢复保留着的通道，积压的消息一连上就交给 OnMessage，保序。
    AppEnv b(env.ep, Opts("app-hot", Transport::kShmRequired));
    CHECK(b.client->Start() == 0);
    CHECK(b.WaitLink(Link::kShm));
    CHECK(b.rec.WaitMessages(3));
    for (std::uint64_t i = 0; i < 3; ++i)
    {
        const std::string p = b.rec.payloads()[i];
        std::uint64_t seq = 0;
        CHECK(Verify(reinterpret_cast<const std::byte*>(p.data()), static_cast<std::uint32_t>(p.size()), &seq));
        CHECK(seq == 100 + i);
    }
    CHECK(WaitUntil([&] { return env.server->stats().resumed == 1; }));
    SendMany(b, "y", 3, 3);
    b.ExpectEchoes("y", 3, 3);
}

void TestPauseAndResumeRead()
{
    LeakCheck leak;
    Env env(Ret(), true);
    EchoOnUds(env);
    AppEnv app(env.ep, Opts("app-p", Transport::kShmRequired));
    CHECK(app.client->Start() == 0);
    CHECK(app.WaitLink(Link::kShm));
    const ConnectionHandle h = app.client->handle();
    h.PauseRead();
    std::this_thread::sleep_for(50ms); // 让暂停先生效
    for (std::uint64_t i = 0; i < 5; ++i)
        CHECK(env.AgentPush("app-p", i, LenOf(i)));
    std::this_thread::sleep_for(200ms);
    CHECK(app.rec.message_count() == 0); // 暂停期间不读，留在环里
    h.ResumeRead();
    CHECK(app.rec.WaitMessages(5)); // 恢复之后一条不少
    CHECK(app.client->stats().rx_messages == 5);
}

// 在回调里暂停读：之后一条都不能再交付（包括已经让出、排着队的续读任务），恢复之后才继续，一条不少。
void TestPauseFromHandlerStopsImmediately()
{
    LeakCheck leak;
    Env env(Ret(), true);
    EchoOnUds(env);
    AgentClientOptions o = Opts("pause-in-cb", Transport::kShmRequired);
    o.rx_budget = 2; // 20 条积压要分好几轮读，暂停时一定有续读任务排着
    std::atomic<bool> paused_once{false}; // 同上：先于 app 声明
    AppEnv app(env.ep, std::move(o));
    app.rec.on_message = [&](IConnection& c, std::string_view)
    {
        if (app.rec.message_count() == 3 && !paused_once.exchange(true))
            c.PauseRead();
    };
    CHECK(app.client->Start() == 0);
    CHECK(app.WaitLink(Link::kShm));
    // 先暂停起来再灌：让 20 条一次性攒在环里。
    app.client->handle().PauseRead();
    std::this_thread::sleep_for(50ms);
    for (std::uint64_t i = 0; i < 20; ++i)
        CHECK(env.AgentPush("pause-in-cb", i, LenOf(i)));
    app.client->handle().ResumeRead(); // 恢复：开始读，第 3 条的回调里再暂停
    CHECK(WaitUntil([&] { return paused_once.load(); }));
    std::this_thread::sleep_for(300ms);
    CHECK(app.rec.message_count() == 3); // 暂停之后一条都不再交付
    app.client->handle().ResumeRead();
    CHECK(app.rec.WaitMessages(20));
}

// 连接一关（还没回收），经句柄跨线程发送就要得到 Closed，不能再收下。
void TestSendAfterCloseIsRejected()
{
    LeakCheck leak;
    Env env(Ret(), true);
    EchoOnUds(env);
    std::atomic<int> after_close{-1}; // 被回调捕获的局部变量要声明在 app 之前：app 析构的过程中回调还可能被触发
    AppEnv app(env.ep, Opts("close-in-cb", Transport::kShmRequired));
    app.rec.on_message = [&](IConnection& c, std::string_view)
    {
        const ConnectionHandle h = c.handle();
        c.Close(); // 标记关闭，回收在本轮分发之后
        after_close.store(static_cast<int>(h.Send(Buf("late"))));
    };
    CHECK(app.client->Start() == 0);
    CHECK(app.WaitLink(Link::kShm));
    CHECK(app.SendStr("trigger"));
    CHECK(WaitUntil([&] { return after_close.load() != -1; }));
    CHECK(after_close.load() == static_cast<int>(SendResult::Closed));
    // 自己关的：之后按退避重新 Attach（不是 Stop）。
    CHECK(app.rec.WaitClosed(0).reason == CloseReason::LocalClose);
    CHECK(WaitUntil([&] { return app.rec.connected_count() == 2; }, 10s)); // 新的连接（Agent 保留着原来的通道，恢复）
    CHECK(app.WaitLink(Link::kShm));
}

// Agent 一侧把 Tx 环的 read_idx 写坏（越过 write_idx）：App 写环时校验不过，按协议错误关闭这条连接，再走全新的 Attach。
// 生产者只在"看起来放不下"时才重读 read_idx，而 Agent 一消费就会把它写回正常值，所以要先让 Agent 停止消费，再写满环。
void TestCorruptTxRingClosesConnection()
{
    LeakCheck leak;
    Env env(Ret(), true);
    EchoOnUds(env);
    AppEnv app(env.ep, Opts("corrupt-tx", Transport::kShmRequired));
    CHECK(app.client->Start() == 0);
    CHECK(app.WaitLink(Link::kShm));
    env.handler.stall_readable.store(true); // Agent 不再消费 Tx
    env.OnLoop(
        [&]
        {
            ShmServerChannel* c = env.handler.FindLive("corrupt-tx");
            CHECK(c != nullptr);
            c->shm().tx_control().read_idx.store(~0ULL / 2); // Agent 才是 read_idx 的写者；写成越过 write_idx 的值
            return 0;
        });
    const std::string chunk(4000, 'p');
    for (int i = 0; i < 400 && app.rec.closed_count() == 0; ++i)
    {
        while (static_cast<std::uint64_t>(i) > app.client->stats().tx_messages + app.client->stats().tx_full_drops + 30 &&
               app.rec.closed_count() == 0)
            std::this_thread::yield();
        (void)app.client->Send(Buf(chunk)); // 写满之后的 TryReserve 重读 read_idx，发现越界
    }
    CHECK(app.rec.WaitClosed(0).reason == CloseReason::ProtocolError);
    env.handler.stall_readable.store(false);
    // Agent 保留着的是同一块被写坏的内存：重新 Attach 带上 discard_retained 丢弃它重建，而不是"恢复"一块坏内存卡死。
    CHECK(WaitUntil([&] { return app.rec.connected_count() == 2; }, 10s));
    CHECK(app.WaitLink(Link::kShm));
    CHECK(env.server->stats().discarded == 1 && env.server->stats().resumed == 0);
}

void TestStartBeforeAgentExists()
{
    LeakCheck leak;
    TempDir dir;
    const Endpoint ep = dir.Unix("agent.sock");
    AppEnv app(ep, Opts("late", Transport::kShmPreferred));
    CHECK(app.client->Start() == 0);
    CHECK(WaitUntil([&] { return app.Failures().size() >= 3; })); // Agent 还没起来：连不上，按退避重试
    for (const auto& [e, retry] : app.Failures())
    {
        CHECK(e == ENOENT || e == ECONNREFUSED);
        CHECK(retry);
    }
    CHECK(app.client->link() == Link::kNone && app.client->Send(Buf("x")) == SendResult::Closed);
    Env agent(Ret(), ep, true);
    CHECK(app.WaitLink(Link::kShm, 10s));
    CHECK(app.client->stats().attaches == 1 && app.client->stats().attach_failures >= 3);
}

void TestDuplicateBusIdRetriesUntilFree()
{
    LeakCheck leak;
    Env env(Ret(), true);
    EchoOnUds(env);
    AppEnv first(env.ep, Opts("dup", Transport::kShmRequired));
    CHECK(first.client->Start() == 0);
    CHECK(first.WaitLink(Link::kShm));
    AppEnv second(env.ep, Opts("dup", Transport::kShmRequired));
    CHECK(second.client->Start() == 0);
    CHECK(WaitUntil([&] { return second.Failures().size() >= 3; }));
    for (const auto& [e, retry] : second.Failures())
        CHECK(e == EBUSY && retry); // 名字被占：可重试
    CHECK(second.client->link() == Link::kNone);
    first.client->Stop(); // Bye：Agent 立刻销毁，名字放出来
    CHECK(second.WaitLink(Link::kShm, 10s));
    CHECK(second.SendStr("z") && second.rec.WaitMessages(1));
}

// 多个业务线程同时经 INetClient::Send 发：都汇到所属 IO 线程写环，一条不少（发送方按已收到的数流控，免得环满丢）。
void TestConcurrentSenders()
{
    LeakCheck leak;
    for (const Transport tr : {Transport::kShmRequired, Transport::kUdsOnly})
    {
        Env env(Ret(), true);
        EchoOnUds(env);
        AppEnv app(env.ep, Opts("conc", tr));
        CHECK(app.client->Start() == 0);
        CHECK(app.WaitLink(tr == Transport::kUdsOnly ? Link::kUds : Link::kShm));
        constexpr int kThreads = 4;
        constexpr int kPer = 3000;
        std::atomic<std::uint64_t> sent{0};
        std::vector<std::thread> ts;
        for (int t = 0; t < kThreads; ++t)
        {
            ts.emplace_back(
                [&, t]
                {
                    for (int i = 0; i < kPer; ++i)
                    {
                        while (sent.load() > app.rec.message_count() + 3000)
                            std::this_thread::yield();
                        CHECK(app.client->Send(Buf("t" + std::to_string(t) + "-" + std::to_string(i))) == SendResult::Ok);
                        sent.fetch_add(1);
                    }
                });
        }
        for (auto& th : ts)
            th.join();
        CHECK(app.rec.WaitMessages(kThreads * kPer, 20s));
        // 每个线程自己的消息保序。
        std::array<int, kThreads> next{};
        for (const std::string& p : app.rec.payloads())
        {
            const int t = p[1] - '0';
            CHECK(std::stoi(p.substr(3)) == next[static_cast<std::size_t>(t)]++);
        }
        for (int n : next)
            CHECK(n == kPer);
        CHECK(app.client->stats().tx_full_drops == 0);
    }
}

void TestTooLargeMessageIsDropped()
{
    LeakCheck leak;
    Env env(Ret(), true);
    EchoOnUds(env);
    AppEnv app(env.ep, Opts("big", Transport::kShmRequired));
    CHECK(app.client->Start() == 0);
    CHECK(app.WaitLink(Link::kShm));
    CHECK(app.client->Send(Buf(std::string(100 * 1024, 'x'))) == SendResult::Ok); // 超过 Tx 环的单条上限（64 KiB）
    CHECK(WaitUntil([&] { return app.client->stats().tx_too_large == 1; }));
    CHECK(app.SendStr("small") && app.rec.WaitMessages(1)); // 连接没受影响
    CHECK(app.rec.payloads()[0] == "small");
}


// 读预算只有 2：每次最多交 2 条就要让出、下一轮再来；500 条一条不少、保序。
void TestSmallRxBudget()
{
    LeakCheck leak;
    Env env(Ret(), true);
    EchoOnUds(env);
    AgentClientOptions o = Opts("budget", Transport::kShmRequired);
    o.rx_budget = 2;
    AppEnv app(env.ep, std::move(o));
    CHECK(app.client->Start() == 0);
    CHECK(app.WaitLink(Link::kShm));
    SendMany(app, "b", 0, 500);
    app.ExpectEchoes("b", 0, 500);
}

// Agent 不在时 Tx 环写满：丢并计数，连接不受影响。跨线程的 Send 写环是异步的：提示置位之前已投递的返回 Ok 却被丢（进 tx_full_drops），
// 提示置位期间（kFullHintWindow）的当场返回 Full（没入队，进 NetStats::send_full）。两类互不重叠，合起来就是发出的每一条。
void TestTxFullWhileAgentDownDropsAndCounts()
{
    LeakCheck leak;
    TempDir dir;
    const Endpoint ep = dir.Unix("agent.sock");
    auto agent = std::make_unique<Env>(Ret(), ep, true);
    AppEnv app(ep, Opts("full", Transport::kShmRequired));
    CHECK(app.client->Start() == 0);
    CHECK(app.WaitLink(Link::kShm));
    agent.reset();
    CHECK(WaitUntil([&] { return app.client->agent_down(); }));
    const std::string chunk(4000, 'f');
    std::uint64_t accepted = 0, rejected = 0;
    for (int i = 0; i < 400; ++i) // 1.6 MB > 1 MiB 的 Tx 环
    {
        // 跨线程发送有自己的背压（发送方预扣的 pending_bytes 超过水位就按慢消费者拒绝）：按 IO 线程已处理的数量分批发。
        while (accepted > app.client->stats().tx_messages + app.client->stats().tx_full_drops + 30)
            std::this_thread::yield();
        const SendResult r = app.client->Send(Buf(chunk));
        CHECK(r == SendResult::Ok || r == SendResult::Full); // 连接始终开着：不会是 Closed
        ++(r == SendResult::Ok ? accepted : rejected);
    }
    CHECK(WaitUntil([&] { return app.client->stats().tx_messages + app.client->stats().tx_full_drops == accepted; }));
    const AgentClientStats st = app.client->stats();
    CHECK(st.tx_full_drops > 0 && st.tx_messages > 100); // 1 MiB 的环放得下约 260 条 4 KB 的
    CHECK(app.group.GetStats().send_full == rejected);   // Full 当场返回的，不进 tx_messages / tx_full_drops
    CHECK(app.rec.closed_count() == 0 && app.client->link() == Link::kShm);
    // 新 Agent 来了：环里排着的（没被丢的）照常处理。
    agent = std::make_unique<Env>(Ret(), ep, true);
    EchoOnUds(*agent);
    CHECK(WaitUntil([&] { return !app.client->agent_down(); }, 10s));
    CHECK(app.rec.WaitMessages(st.tx_messages, 10s));
}

// 环满时明说"没发出"：IO 线程上直接 Send 得到 Full（连接还开着，只进 tx_full_drops）；经句柄跨线程发在提示置位期间当场得到 Full，
// 不入队、不占 pending_bytes；Agent 腾出空间、提示过期之后又能发。
// 满环与对 Full 的检查都在同一个 IO 线程任务里做：2 ms 的提示定时器要等任务返回、loop 转下一轮才会跑，所以结果确定。
void TestTxFullReturnsFullAndRecovers()
{
    LeakCheck leak;
    Env env(Ret(), true);
    EchoOnUds(env);
    AppEnv app(env.ep, Opts("full-hint", Transport::kShmRequired));
    std::atomic<IConnection*> conn{nullptr}; // 只在 IO 线程上用；连接在 Reattach 之外不会换
    app.rec.on_connected = [&](IConnection& c) { conn.store(&c); };
    CHECK(app.client->Start() == 0);
    CHECK(app.WaitLink(Link::kShm));
    CHECK(WaitUntil([&] { return conn.load() != nullptr; }));

    env.handler.stall_readable.store(true); // Agent 不再消费 Tx：写满之后不会自己腾出空间
    EventLoop& loop = GroupAccess::PickLoop(app.group);
    const std::string chunk(4000, 'f');
    int sent_ok = 0;
    const std::uint64_t full_before = app.group.GetStats().send_full;
    loop.CallSync(
        [&]
        {
            IConnection& c = *conn.load();
            SendResult r = SendResult::Ok;
            while (r == SendResult::Ok && sent_ok < 1000) // 1 MiB 的环放得下约 260 条
            {
                r = c.Send(Buf(chunk));
                sent_ok += r == SendResult::Ok ? 1 : 0;
            }
            CHECK(r == SendResult::Full && sent_ok > 100 && c.IsConnected());
            // 提示已置位：同一线程经句柄 / 经 AgentClient 发，当场 Full，没有入队，也没占槽的 pending_bytes。
            CHECK(c.handle().Send(Buf("x")) == SendResult::Full);
            CHECK(app.client->Send(Buf("y")) == SendResult::Full);
            CHECK(loop.table().PendingBytes(c.handle().slot()) == 0);
            return 0;
        });
    const AgentClientStats st = app.client->stats();
    CHECK(st.tx_messages == static_cast<std::uint64_t>(sent_ok) && st.tx_full_drops == 1); // 直发的那一次 Full 算一条丢弃
    CHECK(app.group.GetStats().send_full == full_before + 2);                              // 两次当场拒绝单独计，不进 tx_full_drops
    CHECK(app.rec.closed_count() == 0);

    // Agent 恢复消费：排着的全部回显回来（说明 Tx 环已经排空），提示也早已过期（定时器 2 ms），于是又能发了。
    env.handler.stall_readable.store(false);
    CHECK(app.rec.WaitMessages(static_cast<std::size_t>(sent_ok), 10s));
    CHECK(WaitUntil([&] { return app.client->Send(Buf("again")) == SendResult::Ok; }, 5s));
    CHECK(app.rec.WaitMessages(static_cast<std::size_t>(sent_ok) + 1, 5s));
    CHECK(app.rec.payloads().back() == "again");
    CHECK(app.client->stats().tx_full_drops == 1 && app.rec.closed_count() == 0);
}

// 经 ConnectionHandle 发的每条消息，发送方在槽上预扣的 pending_bytes 要由连接还回去：累计发送量远超水位（默认 256 KiB）
// 也不能被当成慢消费者关掉。
void TestSendAccountingDoesNotLeakPending()
{
    LeakCheck leak;
    Env env(Ret(), true);
    EchoOnUds(env);
    AppEnv app(env.ep, Opts("acct", Transport::kShmRequired));
    CHECK(app.client->Start() == 0);
    CHECK(app.WaitLink(Link::kShm));
    constexpr int kN = 6000;
    const std::string body(100, 'p'); // 6000 × 100 B = 600 KB，是水位的两倍多
    for (int i = 0; i < kN; ++i)
    {
        while (static_cast<std::size_t>(i) > app.rec.message_count() + 500)
            std::this_thread::yield(); // 在途不超过 500 条：免得环满丢
        CHECK(app.client->Send(Buf(body)) == SendResult::Ok);
    }
    CHECK(app.rec.WaitMessages(kN));
    CHECK(app.rec.closed_count() == 0);
}

// discard_retained：丢弃 Agent 保留着的同名队列，不接手积压。
void TestDiscardRetainedOption()
{
    LeakCheck leak;
    Env env(Ret(), true);
    EchoOnUds(env);
    {
        AppEnv a(env.ep, Opts("disc", Transport::kShmRequired));
        CHECK(a.client->Start() == 0);
        CHECK(a.WaitLink(Link::kShm));
        a.client->PrepareRestart(10s);
        std::this_thread::sleep_for(100ms);
    }
    CHECK(WaitUntil([&] { return env.server->stats().detached_channels == 1; }));
    for (std::uint64_t i = 0; i < 3; ++i)
        CHECK(env.AgentPush("disc", i, LenOf(i)));
    AgentClientOptions o = Opts("disc", Transport::kShmRequired);
    o.discard_retained = true;
    AppEnv b(env.ep, std::move(o));
    CHECK(b.client->Start() == 0);
    CHECK(b.WaitLink(Link::kShm));
    CHECK(WaitUntil([&] { return env.server->stats().discarded == 1; }));
    std::this_thread::sleep_for(200ms);
    CHECK(b.rec.message_count() == 0); // 积压被丢弃了
    CHECK(env.server->stats().resumed == 0);
}

// Stop 之后不再重试：Agent 不在、客户端在退避重试时 Stop，失败回调的次数不再增加。
void TestStopCancelsRetries()
{
    LeakCheck leak;
    TempDir dir;
    AppEnv app(dir.Unix("agent.sock"), Opts("stop", Transport::kShmPreferred));
    CHECK(app.client->Start() == 0);
    CHECK(WaitUntil([&] { return app.Failures().size() >= 2; }));
    app.client->Stop();
    std::this_thread::sleep_for(100ms); // 让 Stop 的任务先执行
    const std::size_t n = app.Failures().size();
    std::this_thread::sleep_for(600ms); // 退避上限 200ms：要是还在重试，这段时间里至少再失败几次
    CHECK(app.Failures().size() == n);
    // 再 Start 又能跑。
    Env agent(Ret(), dir.Unix("agent.sock"), true);
    CHECK(app.client->Start() == 0);
    CHECK(app.WaitLink(Link::kShm, 10s));
}

// Reattach 被 Agent 判定不可能成功（这回新 Agent 的单环上限比手里的环还小）：关闭这条连接，再走一遍全新的 Attach
// （全新的 Attach 同样被拒、不可重试，回到 Idle）。
void TestReattachRejectedClosesConnection()
{
    LeakCheck leak;
    TempDir dir;
    const Endpoint ep = dir.Unix("agent.sock");
    auto agent = std::make_unique<Env>(Ret(), ep, true);
    AgentClientOptions o = Opts("big-ring", Transport::kShmRequired);
    o.tx_capacity = 2 * shm::kMinRingCapacity;
    o.rx_capacity = 2 * shm::kMinRingCapacity;
    AppEnv app(ep, std::move(o));
    CHECK(app.client->Start() == 0);
    CHECK(app.WaitLink(Link::kShm));
    agent.reset();
    CHECK(WaitUntil([&] { return app.client->agent_down(); }));
    ShmServerOptions small = Ret();
    small.max_ring_capacity = shm::kMinRingCapacity; // 手里的环（2 MiB）超过它：Reattach 的校验过不了
    agent = std::make_unique<Env>(small, ep, true);
    const auto closed = app.rec.WaitClosed(0, 10s); // 连接被关闭
    CHECK(closed.reason == CloseReason::LocalClose);
    CHECK(app.WaitLink(Link::kNone));
    CHECK(WaitUntil([&] { return !app.Failures().empty(); }, 10s)); // 之后的全新 Attach 同样被拒
    CHECK(app.Failures().back().first == EINVAL && !app.Failures().back().second);
    CHECK(app.client->stats().reattach_failures >= 1);
}

// 恶意 / 出错的 Agent：回复里各种不对，客户端都当协议错误（EPROTO，不可重试），不会建出连接。
void TestHostileAgentReplies()
{
    LeakCheck leak;
    struct Case
    {
        const char* name;
        Transport asked;
        std::function<void(wire::Reply&)> mutate;
        std::size_t nfds;       // 随回复附带几个 fd（0 / 3 / 其它）
        int expect_errno;       // 失败回调里的 errno
        bool expect_retry;
    };
    const std::vector<Case> cases = {
        {"回复要共享内存却只带 2 个 fd", Transport::kShmRequired, [](wire::Reply&) {}, 2, EPROTO, false},
        {"回复的布局对不上", Transport::kShmRequired, [](wire::Reply& r) { r.total_size += 4096; }, 3, EPROTO, false},
        {"回复的 resumed 取值不对", Transport::kShmRequired, [](wire::Reply& r) { r.resumed = 2; }, 3, EPROTO, false},
        {"要仅 UDS 却回了共享内存", Transport::kUdsOnly, [](wire::Reply&) {}, 3, EPROTO, false},
        {"要共享内存（必须）却被退化成仅 UDS", Transport::kShmRequired,
         [](wire::Reply& r) { r.transport = static_cast<std::uint32_t>(wire::ChosenTransport::kUds); }, 0, EPROTO, false},
        {"仅 UDS 的回复带了 fd", Transport::kUdsOnly,
         [](wire::Reply& r) { r.transport = static_cast<std::uint32_t>(wire::ChosenTransport::kUds); }, 3, EPROTO, false},
        {"回复的传输方式是 0", Transport::kShmPreferred, [](wire::Reply& r) { r.transport = 0; }, 3, EPROTO, false},
        {"Reject(ENOSPC)", Transport::kShmRequired, [](wire::Reply& r) { r.result = ENOSPC; }, 0, ENOSPC, true},
        {"Reject(EACCES)", Transport::kShmRequired, [](wire::Reply& r) { r.result = EACCES; }, 0, EACCES, false},
    };
    for (const Case& c : cases)
    {
        TempDir dir;
        const Endpoint ep = dir.Unix("fake.sock");
        FakeAgent fake(ep,
                       [&c](int conn, const wire::Hello&)
                       {
                           shm::ShmChannel ch = NewShm();
                           (void)ch.BumpAgentEpoch();
                           (void)ch.BumpAppEpoch();
                           wire::Reply r = GoodReply(ch);
                           c.mutate(r);
                           std::vector<int> fds;
                           const std::array<int, 3> all{ch.memfd(), ch.tx_eventfd(), ch.rx_eventfd()};
                           for (std::size_t i = 0; i < c.nfds && i < 3; ++i)
                               fds.push_back(all[i]);
                           SendReply(conn, r, fds);
                       });
        AppEnv app(ep, Opts("hostile", c.asked));
        CHECK(app.client->Start() == 0);
        if (!WaitUntil([&] { return !app.Failures().empty(); }))
        {
            std::fprintf(stderr, "用例没有失败回调：%s\n", c.name);
            std::abort();
        }
        const auto f = app.Failures().front();
        if (f.first != c.expect_errno || f.second != c.expect_retry)
        {
            std::fprintf(stderr, "用例 [%s]：errno=%d 重试=%d，期望 errno=%d 重试=%d\n", c.name, f.first, f.second,
                         c.expect_errno, c.expect_retry);
            std::abort();
        }
        CHECK(app.client->link() == Link::kNone && app.rec.connected_count() == 0);
        app.client->Stop(); // 可重试的用例会一直试：停掉
    }
}

// Agent 接受了连接却一直不回复：握手要受 handshake_timeout 限制（ETIMEDOUT，可重试），不能一直挂着。
void TestHandshakeTimeout()
{
    LeakCheck leak;
    TempDir dir;
    const Endpoint ep = dir.Unix("fake.sock");
    std::atomic<bool> release{false};
    FakeAgent fake(ep,
                   [&release](int, const wire::Hello&)
                   {
                       while (!release.load())
                           std::this_thread::sleep_for(10ms); // 不回复，也不关
                   });
    AgentClientOptions o = Opts("slow-agent", Transport::kShmRequired);
    o.handshake_timeout = 300ms;
    AppEnv app(ep, std::move(o));
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(app.client->Start() == 0);
    CHECK(WaitUntil([&] { return !app.Failures().empty(); }, 5s));
    const auto waited = std::chrono::steady_clock::now() - t0;
    CHECK(waited >= 250ms && waited < 3s);
    CHECK(app.Failures()[0].first == ETIMEDOUT && app.Failures()[0].second);
    app.client->Stop();
    release.store(true);
}

// AgentServer 要求 group 只有一个 loop（路由器是单线程的）。
void TestAgentServerRequiresSingleLoop()
{
    LeakCheck leak;
    TempDir dir;
    StaticRouteTable table;
    EventLoopGroup two(EventLoopGroupOptions{2, 16});
    CHECK(two.Start() == 0);
    AgentServer bad(two, dir.Unix("a.sock"), table);
    CHECK(bad.Start() == EINVAL);
    EventLoopGroup one(EventLoopGroupOptions{1, 16});
    CHECK(one.Start() == 0);
    AgentServer good(one, dir.Unix("b.sock"), table);
    CHECK(good.Start() == 0);
}

// Agent 把"保留着的"内存还给我们（resumed = 1）但它校验不过（环被写坏）：不是不可重试的配置错误，下一次 Attach 自动带
// discard_retained 丢弃重建；丢弃只用这一次，之后的 Attach 不再带。
void TestResumedMemoryInvalidTriggersDiscard()
{
    LeakCheck leak;
    TempDir dir;
    const Endpoint ep = dir.Unix("fake.sock");
    auto bad = std::make_shared<shm::ShmChannel>(NewShm());
    (void)bad->BumpAgentEpoch();
    (void)bad->BumpAppEpoch();
    bad->tx_control().write_idx.store(bad->layout().tx.capacity + 4096); // 占用超过容量：接手校验不过
    auto good = std::make_shared<shm::ShmChannel>(NewShm());
    (void)good->BumpAgentEpoch();
    (void)good->BumpAppEpoch();
    const auto reply_with = [](std::shared_ptr<shm::ShmChannel> ch, std::uint32_t resumed)
    {
        return [ch, resumed](int conn, const wire::Hello&)
        {
            wire::Reply r = GoodReply(*ch);
            r.resumed = resumed;
            const std::array<int, 3> fds{ch->memfd(), ch->tx_eventfd(), ch->rx_eventfd()};
            SendReply(conn, r, fds);
            std::this_thread::sleep_for(500ms); // 连接留着，别让客户端马上去 Reattach
        };
    };
    FakeAgent fake(ep,
                   std::vector<FakeAgent::Behavior>{reply_with(bad, 1), reply_with(good, 0), reply_with(good, 0)});
    AppEnv app(ep, Opts("resumed-bad", Transport::kShmRequired));
    CHECK(app.client->Start() == 0);
    CHECK(app.WaitLink(Link::kShm, 10s)); // 第二次 Attach 成功
    const auto f = app.Failures();
    CHECK(f.size() == 1 && f[0].first == EINVAL && f[0].second); // 可重试，不是停在 Idle
    const auto hellos = fake.hellos();
    CHECK(hellos.size() == 2);
    CHECK((hellos[0].flags & wire::kFlagDiscardRetained) == 0);
    CHECK((hellos[1].flags & wire::kFlagDiscardRetained) != 0); // 第二次带了丢弃
    // 丢弃只用这一次：之后的 Attach 不再带（除非选项里本来就要）。
    app.client->Stop();
    CHECK(app.rec.WaitClosed(0).reason == CloseReason::LocalClose);
    CHECK(app.client->Start() == 0);
    CHECK(WaitUntil([&] { return fake.hellos().size() == 3; }, 10s));
    CHECK((fake.hellos()[2].flags & wire::kFlagDiscardRetained) == 0);
    app.client->Stop();
}

// 连接成功之后退避回到初始值：Agent 不在时失败几次、等待越来越长；连上之后 Agent 一侧把这条连接关掉（Agent 一直在，
// 所以客户端的第一次重试必然成功），那次退避等了多久就是 last_backoff_ms——应当是 initial_delay，不是失败时涨起来的值。
void TestBackoffResetsAfterSuccess()
{
    LeakCheck leak;
    TempDir dir;
    const Endpoint ep = dir.Unix("agent.sock");
    AppEnv app(ep, Opts("backoff", Transport::kUdsOnly)); // reconnect = {20ms, 200ms, 2.0, 无抖动}
    CHECK(app.client->Start() == 0);
    CHECK(WaitUntil([&] { return app.Failures().size() >= 5; }));
    CHECK(app.client->stats().last_backoff_ms >= 160); // 20, 40, 80, 160, 200 ……
    Env agent(Ret(), ep, true);
    std::mutex mu;
    ConnectionHandle agent_side;
    agent.uds_events.on_connected = [&](IConnection& c)
    {
        const std::lock_guard lk(mu);
        agent_side = c.handle();
    };
    CHECK(app.WaitLink(Link::kUds, 10s));
    CHECK(WaitUntil(
        [&]
        {
            const std::lock_guard lk(mu);
            return agent_side.valid();
        }));
    {
        const std::lock_guard lk(mu);
        agent_side.Close(); // Agent 一侧关掉：客户端看到 PeerClosed，按退避重新握手
    }
    CHECK(app.rec.WaitClosed(0, 10s).reason == CloseReason::PeerClosed);
    CHECK(WaitUntil([&] { return app.client->stats().attaches == 2; }, 10s));
    CHECK(app.client->stats().last_backoff_ms == 20); // 第一次（也是唯一一次）重试用的是初始的 20ms
    app.client->Stop();
}

// 反复 Stop / Start：旧连接的 OnClosed 要等回收阶段才到，不能被当成新一轮的连接关闭（清掉新握手、多触发重试）。
void TestRapidStopStart()
{
    LeakCheck leak;
    Env env(Ret(), true);
    EchoOnUds(env);
    for (const Transport tr : {Transport::kShmRequired, Transport::kUdsOnly})
    {
        AppEnv app(env.ep, Opts("flap-" + std::to_string(static_cast<int>(tr)), tr));
        CHECK(app.client->Start() == 0);
        CHECK(app.WaitLink(tr == Transport::kUdsOnly ? Link::kUds : Link::kShm));
        for (int i = 0; i < 30; ++i)
        {
            app.client->Stop();
            CHECK(app.client->Start() == 0); // 立刻再来：此时上一条连接多半还没回收
        }
        // Stop / Start 只是往 IO 线程投任务：先用一个同步任务等它们都执行完，再等新的连接（否则看到的还是旧连接的状态）。
        (void)GroupAccess::PickLoop(app.group).CallSync([] { return 0; });
        CHECK(WaitUntil([&] { return app.rec.connected_count() == 2 && app.client->handle().valid(); }, 10s));
        CHECK(app.WaitLink(tr == Transport::kUdsOnly ? Link::kUds : Link::kShm));
        // 最终稳定在一条连接上，业务照常收发。
        const std::size_t base = app.rec.message_count();
        CHECK(app.SendStr("after-flap"));
        CHECK(app.rec.WaitMessages(base + 1, 10s));
        // 每条连接的事件都符合 C M* X? 的次序（没有重复的 OnClosed）。
        app.rec.CheckInvariants();
        CHECK(app.rec.closed_count() + 1 >= app.rec.connected_count()); // 至多一条还活着
    }
}

// 假 Agent 先正常握手、随后断开（客户端去 Reattach），对 Reattach 回各种坏回复：客户端判定这条通道没救了，
// 关闭连接（OnClosed）、再走全新的 Attach；回复正常时则恢复成功、连接不关。
void TestHostileReattachReplies()
{
    LeakCheck leak;
    struct Case
    {
        const char* name;
        std::function<void(wire::Reply&, shm::ShmChannel&)> mutate;
        bool ok; // 回复是对的：恢复成功
        bool with_fds = false; // 回复里附带 fd（Reattach 的 Accept 不该带）
    };
    const std::vector<Case> cases = {
        {"正常", [](wire::Reply&, shm::ShmChannel&) {}, true},
        {"agent_epoch 变大（Agent 重启了）", [](wire::Reply& r, shm::ShmChannel&) { r.agent_epoch = 9; }, true},
        {"agent_epoch 倒退", [](wire::Reply& r, shm::ShmChannel&) { r.agent_epoch = 0; }, false},
        {"回复里的 app_epoch 对不上（被接管）", [](wire::Reply& r, shm::ShmChannel&) { r.app_epoch = 2; }, false},
        {"共享内存头里的 app_epoch 被改了（被接管），回复里没变", [](wire::Reply&, shm::ShmChannel& ch) { (void)ch.BumpAppEpoch(); }, false},
        {"resumed 不是 1", [](wire::Reply& r, shm::ShmChannel&) { r.resumed = 0; }, false},
        {"Reattach 的回复带了 fd", [](wire::Reply& r, shm::ShmChannel&) { r.reserved = 0; }, false, true},
        {"布局对不上", [](wire::Reply& r, shm::ShmChannel&) { r.total_size += 4096; }, false},
        {"传输方式不是共享内存", [](wire::Reply& r, shm::ShmChannel&) {
             r.transport = static_cast<std::uint32_t>(wire::ChosenTransport::kUds);
         }, false},
    };
    for (const Case& c : cases)
    {
        TempDir dir;
        const Endpoint ep = dir.Unix("fake.sock");
        auto ch = std::make_shared<shm::ShmChannel>(NewShm());
        (void)ch->BumpAgentEpoch();
        (void)ch->BumpAppEpoch();
        const FakeAgent::Behavior first = [ch](int conn, const wire::Hello&)
        {
            const std::array<int, 3> fds{ch->memfd(), ch->tx_eventfd(), ch->rx_eventfd()};
            SendReply(conn, GoodReply(*ch), fds); // 然后断开：客户端要去 Reattach
        };
        const FakeAgent::Behavior reattach = [ch, &c](int conn, const wire::Hello& h)
        {
            CHECK(h.attach_type == static_cast<std::uint32_t>(wire::AttachType::kReattach));
            wire::Reply r = GoodReply(*ch);
            r.resumed = 1;
            c.mutate(r, *ch);
            const std::array<int, 3> fds{ch->memfd(), ch->tx_eventfd(), ch->rx_eventfd()};
            SendReply(conn, r, c.with_fds ? std::span<const int>(fds) : std::span<const int>());
            if (c.ok)
                std::this_thread::sleep_for(300ms); // 回复正确：连接留着，别马上断（断了客户端又要 Reattach）
        };
        std::vector<FakeAgent::Behavior> behaviors{first, reattach};
        if (!c.ok)
            behaviors.push_back(first); // 失败后客户端关闭连接、自动重新 Attach：这次给它一个正常的
        FakeAgent fake(ep, std::move(behaviors));
        AppEnv app(ep, Opts("hostile-reattach", Transport::kShmRequired));
        CHECK(app.client->Start() == 0);
        if (c.ok)
        {
            CHECK(WaitUntil([&] { return app.client->stats().reattaches == 1; }));
            CHECK(app.rec.closed_count() == 0 && app.rec.connected_count() == 1);
        }
        else
        {
            if (!WaitUntil([&] { return app.rec.closed_count() == 1; }))
            {
                std::fprintf(stderr, "用例没有关闭连接：%s\n", c.name);
                std::abort();
            }
            CHECK(app.client->stats().reattach_failures >= 1 && app.client->stats().reattaches == 0);
            CHECK(WaitUntil([&] { return app.rec.connected_count() == 2; }, 10s)); // 再 Attach
        }
        app.client->Stop();
        std::this_thread::sleep_for(50ms);
    }
}

// 两次 Reattach：第一次 agent_epoch 变大（新 Agent），第二次回了个更小的——倒退，说明不对劲。证明第一次成功后确实记下了新值。
void TestAgentEpochDoesNotRegressAcrossReattaches()
{
    LeakCheck leak;
    TempDir dir;
    const Endpoint ep = dir.Unix("fake.sock");
    auto ch = std::make_shared<shm::ShmChannel>(NewShm());
    (void)ch->BumpAgentEpoch();
    (void)ch->BumpAppEpoch();
    const FakeAgent::Behavior first = [ch](int conn, const wire::Hello&)
    {
        const std::array<int, 3> fds{ch->memfd(), ch->tx_eventfd(), ch->rx_eventfd()};
        SendReply(conn, GoodReply(*ch), fds);
    };
    const auto reattach_with = [ch](std::uint64_t agent_epoch)
    {
        return [ch, agent_epoch](int conn, const wire::Hello&)
        {
            wire::Reply r = GoodReply(*ch);
            r.resumed = 1;
            r.agent_epoch = agent_epoch;
            SendReply(conn, r, {});
        };
    };
    FakeAgent fake(ep, std::vector<FakeAgent::Behavior>{first, reattach_with(9), reattach_with(5), first});
    AppEnv app(ep, Opts("epoch-regress", Transport::kShmRequired));
    CHECK(app.client->Start() == 0);
    CHECK(WaitUntil([&] { return app.client->stats().reattaches == 1; }));                   // epoch 1 → 9：恢复成功
    CHECK(app.rec.WaitClosed(0, 10s).reason == CloseReason::LocalClose);                       // 9 → 5：倒退，关闭
    CHECK(app.client->stats().reattach_failures >= 1 && app.client->stats().reattaches == 1);
    CHECK(WaitUntil([&] { return app.rec.connected_count() == 2; }, 10s));
    app.client->Stop();
    std::this_thread::sleep_for(50ms);
}

// 客户端先于它的连接销毁、处理器随后立刻释放（group 还在跑）：之后不能再有回调碰到已释放的东西。asan 下才测得出。
void TestNoCallbacksAfterClientDestroyed()
{
    LeakCheck leak;
    for (int round = 0; round < 4; ++round)
    {
        for (const Transport tr : {Transport::kShmRequired, Transport::kUdsOnly})
        {
            Env env(Ret(), true);
            EchoOnUds(env);
            EventLoopGroup group(EventLoopGroupOptions{1, 64});
            CHECK(group.Start() == 0);
            auto rec = std::make_unique<EventRecorder>();
            // OnClosed 里睡一会儿：把"客户端析构返回时回调还在进行"这个窗口撑开（见 shm_transport_test 的同名用例）。
            rec->on_closed = [](IConnection&, CloseReason) { std::this_thread::sleep_for(30ms); };
            auto client = std::make_unique<AgentClient>(group, env.ep, Opts("life", tr), *rec);
            CHECK(client->Start() == 0);
            CHECK(WaitUntil([&] { return client->link() != Link::kNone; }));
            CHECK(client->Send(Buf("x")) == SendResult::Ok);
            CHECK(rec->WaitMessages(1));
            client.reset(); // 同步销毁：此后不许再有回调
            rec.reset();    // 处理器随即释放
            std::this_thread::sleep_for(20ms);
        }
    }
}

} // namespace

// 环境变量 ONLY=用例名 只跑名字里含它的用例（调试用）。
#define RUN(t)                                                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        const char* only = std::getenv("ONLY");                                                                        \
        if (only != nullptr && std::strstr(#t, only) == nullptr)                                                       \
            break;                                                                                                     \
        std::fprintf(stderr, "[run] %s\n", #t);                                                                        \
        t();                                                                                                           \
    } while (0)

int main()
{
    RUN(TestEchoOnBothTransports);
    RUN(TestNegotiationOutcomes);
    RUN(TestAgentRestartKeepsShmConnection);
    RUN(TestAgentRestartUdsOnlyReconnects);
    RUN(TestAppRestartResumesBacklog);
    RUN(TestPauseAndResumeRead);
    RUN(TestPauseFromHandlerStopsImmediately);
    RUN(TestSendAfterCloseIsRejected);
    RUN(TestCorruptTxRingClosesConnection);
    RUN(TestStartBeforeAgentExists);
    RUN(TestDuplicateBusIdRetriesUntilFree);
    RUN(TestConcurrentSenders);
    RUN(TestSmallRxBudget);
    RUN(TestTxFullWhileAgentDownDropsAndCounts);
    RUN(TestTxFullReturnsFullAndRecovers);
    RUN(TestSendAccountingDoesNotLeakPending);
    RUN(TestDiscardRetainedOption);
    RUN(TestStopCancelsRetries);
    RUN(TestReattachRejectedClosesConnection);
    RUN(TestHostileAgentReplies);
    RUN(TestResumedMemoryInvalidTriggersDiscard);
    RUN(TestBackoffResetsAfterSuccess);
    RUN(TestHandshakeTimeout);
    RUN(TestAgentServerRequiresSingleLoop);
    RUN(TestRapidStopStart);
    RUN(TestHostileReattachReplies);
    RUN(TestAgentEpochDoesNotRegressAcrossReattaches);
    RUN(TestTooLargeMessageIsDropped);
    RUN(TestNoCallbacksAfterClientDestroyed);
    std::puts("agent_client_test: OK");
    return 0;
}
