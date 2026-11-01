// 共享内存通道的保留与恢复（设计见 docs/共享内存IPC设计.md §6.2 / §6.3 / §6.5）：
//   App 重启：UDS 断开 → Detached（保留）→ 同 busid 回来恢复 / 到期销毁；Tx 里的遗言被排空；Rx 里攒着的消息重启后读到
//   控制消息：PrepareRestart / Bye
//   Agent 重启：App 带着 fd 来 Reattach；上一任死在处理一条记录的期间时那一条被跳过
//   不可信输入：Reattach 交来的 fd 与内存里的各种坏法
//   跨进程：kill -9 App 后新进程接着用、kill -9 Agent 后 Reattach（消息至多丢一条、不重复、不乱序）
//
// 跨进程的用例在 main 一开头、任何线程创建之前 fork 出子进程（见 shm_channel_test.cpp 文件头）。
// 无测试框架依赖：失败即 abort，并发用例带超时。

#include <algorithm>
#include <condition_variable>
#include <initializer_list>
#include <type_traits>

#include "base/net/testing/shm_channel_test_util.h"

using namespace shmtest;

namespace
{

std::array<int, 3> FdsOf(const shm::ShmChannel& ch) { return {ch.memfd(), ch.tx_eventfd(), ch.rx_eventfd()}; }

ShmServerOptions Ret(std::chrono::milliseconds retain = 30s)
{
    ShmServerOptions o;
    o.retain_default = retain;
    return o;
}

// 已经握手成功的原始连接（不经过 ShmChannelClient，自己拿着 fd）。
struct Raw
{
    UniqueFd fd;
    RawReply reply;
};

Raw RawAttach(const Endpoint& ep, std::string_view busid)
{
    Raw r;
    r.fd = RawConnect(ep);
    const wire::Hello h = MakeHello(busid);
    RawSend(r.fd.get(), &h, sizeof(h));
    auto rep = RawRecvReply(r.fd.get());
    CHECK(rep.has_value() && rep->reply.result == 0 && rep->nfds == 3);
    r.reply = std::move(*rep);
    return r;
}

// 事件序列是回调里记的，晚于服务端的计数，也可能晚于客户端收到回复：等它变成期望的样子再核对。
void ExpectTrace(const EchoHandler& h, const std::string& busid, const std::string& want)
{
    if (!WaitUntil([&] { return h.trace(busid) == want; }))
    {
        std::fprintf(stderr, "trace(%s) = \"%s\"，期望 \"%s\"\n", busid.c_str(), h.trace(busid).c_str(), want.c_str());
        std::abort();
    }
}

// 读一条 Rx，核对序号与内容，然后释放。
void ExpectRx(ShmChannelClient& c, std::initializer_list<std::uint64_t> seqs)
{
    for (const std::uint64_t want : seqs)
    {
        shm::RecordView v;
        CHECK(RecvOne(c, v));
        std::uint64_t seq = 0;
        CHECK(Verify(v.payload, v.len, &seq));
        CHECK(seq == want && v.len == LenOf(seq));
        c.rx().Pop();
    }
}

void ExpectRxEmpty(ShmChannelClient& c)
{
    shm::RecordView v;
    CHECK(c.rx().Peek(v) == shm::PeekStatus::kEmpty);
}

void SendRange(ShmChannelClient& c, std::uint64_t first, std::uint64_t count)
{
    for (std::uint64_t i = 0; i < count; ++i)
        CHECK(SendMsg(c, first + i, LenOf(first + i)));
}

// 一条记录在环里占的字节：16 字节头 + 载荷，向上取整到 64。
std::uint64_t RecordBytes(std::uint64_t seq) { return (16 + LenOf(seq) + 63) / 64 * 64; }

// ---- App 重启 ----

void TestAppRestartResumes()
{
    LeakCheck leak;
    Env env(Ret());
    std::uint64_t agent_epoch = 0;
    {
        ShmChannelClient a = MustConnect(env.ep, "app-r");
        CHECK(!a.resumed() && a.app_epoch() == 1 && a.agent_epoch() == 1);
        agent_epoch = a.agent_epoch();
        EchoRoundTrips(a, 3);
        // 写 5 条之后"崩溃"：不读回显，对象析构就是关 UDS、关 fd、解映射。
        SendRange(a, 100, 5);
    }
    // 遗言：App 没了，Agent 仍把已经提交的 5 条处理完（回显进 Rx）。
    CHECK(WaitUntil([&] { return env.handler.echoed.load() == 3 + 5; }));
    CHECK(WaitUntil([&] { return env.server->stats().detached_channels == 1; }));
    ExpectTrace(env.handler, "app-r", "OD");
    CHECK(env.server->stats().closed == 0);
    // 保留期里 Agent 照样能往 Rx 写（攒着）。
    for (std::uint64_t i = 200; i < 204; ++i)
        CHECK(env.AgentPush("app-r", i, LenOf(i)));

    // 同名的新进程回来：接手同一块内存。
    ShmChannelClient b = MustConnect(env.ep, "app-r");
    CHECK(b.resumed());
    CHECK(b.app_epoch() == 2 && b.agent_epoch() == agent_epoch); // App 的 epoch 加一，Agent 的不变
    CHECK(!b.Superseded());
    // Rx 里先是 5 条遗言的回显，再是 Agent 在保留期里写的 4 条，一条不少、顺序不乱。
    ExpectRx(b, {100, 101, 102, 103, 104, 200, 201, 202, 203});
    ExpectRxEmpty(b);
    // Tx 的游标接着用：新进程发的照常被处理。
    EchoRoundTrips(b, 20, 1000);

    CHECK(WaitUntil(
        [&]
        {
            const ShmServerStats st = env.server->stats();
            return st.opened == 1 && st.resumed == 1 && st.detached_total == 1 && st.detached_channels == 0 &&
                   st.closed == 0;
        }));
    ExpectTrace(env.handler, "app-r", "ODR");
    CHECK(env.handler.events().back().id == env.handler.events().front().id); // 同一条通道，id 不变
}

void TestRetentionExpires()
{
    LeakCheck leak;
    Env env(Ret(1000ms));
    std::uint64_t unread = 0;
    {
        ShmChannelClient a = MustConnect(env.ep, "app-x");
        for (std::uint64_t i = 0; i < 3; ++i)
        {
            CHECK(env.AgentPush("app-x", i, LenOf(i)));
            unread += RecordBytes(i);
        }
    }
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(WaitUntil([&] { return env.server->stats().detached_channels == 1; }));
    std::this_thread::sleep_for(200ms);
    // 保留期内：通道还在，没有被关。
    CHECK(env.handler.trace("app-x") == "OD");
    CHECK(env.server->stats().closed == 0 && env.server->stats().detached_channels == 1);

    CHECK(WaitUntil([&] { return env.server->stats().retention_expired == 1; }, 8s));
    CHECK(std::chrono::steady_clock::now() - t0 >= 800ms); // 不会早于保留时长（留 200ms 余量给 t0 之前的检测延迟）
    CHECK(WaitUntil([&] { return env.handler.trace("app-x") == "ODC"; }));
    CHECK(env.handler.closed_reason("app-x") == ShmCloseReason::RetentionExpired);
    const ShmServerStats st = env.server->stats();
    CHECK(st.detached_channels == 0 && st.open_channels == 0 && st.closed == 1);
    CHECK(st.rx_bytes_discarded == unread); // Rx 里没被读走的字节计入指标

    // 同名再来：全新的通道，没有任何旧东西。
    ShmChannelClient b = MustConnect(env.ep, "app-x");
    CHECK(!b.resumed() && b.app_epoch() == 1 && b.agent_epoch() == 1);
    ExpectRxEmpty(b);
    EchoRoundTrips(b, 5);
}

// 恢复之后又断开：第一次保留的计时器必须作废，否则它会在第二次保留的中途把通道销毁。
// 时间线（保留 2000ms）：t0 第一次断开；t0+500 恢复并随即再断开；
//   旧计时器本该在 t0+2000 触发；第二次保留自己的计时器在 t0+2500 以后触发；在 t0+2250 检查。
void TestStaleRetentionTimerAfterResume()
{
    LeakCheck leak;
    Env env(Ret(2000ms));
    {
        ShmChannelClient a = MustConnect(env.ep, "app-st");
    }
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(WaitUntil([&] { return env.server->stats().detached_channels == 1; }));
    std::this_thread::sleep_for(500ms);
    {
        ShmChannelClient b = MustConnect(env.ep, "app-st");
        CHECK(b.resumed());
    }
    CHECK(WaitUntil([&] { return env.server->stats().detached_total == 2; }));
    std::this_thread::sleep_until(t0 + 2250ms);
    CHECK(env.server->stats().detached_channels == 1 && env.server->stats().retention_expired == 0);
    ExpectTrace(env.handler, "app-st", "ODRD");
    // 第二次保留自己的计时器照常生效。
    CHECK(WaitUntil([&] { return env.server->stats().retention_expired == 1; }, 4s));
}

// 不保留时先排空遗言再销毁，但排空有上限：处理器永远说"还有"（对端还在写、或业务卡住）时不能把通道一直拖着。
void TestDrainTimeoutBoundsStuckChannel()
{
    LeakCheck leak;
    ShmServerOptions o = Ret();
    o.drain_timeout = 300ms;
    Env env(o);
    env.handler.stall_readable.store(true);
    {
        ShmChannelClient a = MustConnect(env.ep, "app-stuck");
        SendRange(a, 0, 3);
        CHECK(a.SendBye() == 0);
    }
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(WaitUntil([&] { return env.handler.closed_reason("app-stuck") == ShmCloseReason::PeerBye; }, 3s));
    CHECK(std::chrono::steady_clock::now() - t0 >= 250ms); // 确实等满了排空超时才放弃
    CHECK(env.handler.echoed.load() == 0);                  // 没排空（处理器一直在说"还有"）
    env.handler.stall_readable.store(false);
}

void TestDrainWithTinyBudgetLosesNothing()
{
    LeakCheck leak;
    Env env(Ret());
    env.handler.budget.store(3); // 每轮只处理 3 条：遗言要靠让出与重来才排得完
    {
        ShmChannelClient a = MustConnect(env.ep, "app-d");
        SendRange(a, 0, 300);
        CHECK(a.SendBye() == 0); // Bye：不保留，排空就销毁
    }
    // 2 秒内就要关：排空完成即销毁；不能靠 5 秒的排空超时兜底。
    CHECK(WaitUntil([&] { return env.handler.closed_reason("app-d") == ShmCloseReason::PeerBye; }, 2s));
    CHECK(env.handler.echoed.load() == 300); // 300 条遗言一条不少
    CHECK(env.handler.trace("app-d") == "OC");
    const ShmServerStats st = env.server->stats();
    CHECK(st.byes == 1 && st.detached_total == 0 && st.open_channels == 0);
}

// ---- 控制消息 ----

void TestPrepareRestart()
{
    LeakCheck leak;
    ShmServerOptions o = Ret(300ms);
    o.retain_max = 2000ms;
    Env env(o);
    // 第 nth 次 Detached 事件的保留时长。事件是 OnDetached 回调里记的，晚于 detached_total 计数，所以要等它出现。
    const auto retain_of = [&](std::size_t nth) -> std::chrono::milliseconds
    {
        std::vector<EchoHandler::Event> ds;
        CHECK(WaitUntil(
            [&]
            {
                ds.clear();
                for (const auto& e : env.handler.events())
                {
                    if (e.kind == EchoHandler::Event::Kind::Detached)
                        ds.push_back(e);
                }
                return ds.size() > nth;
            }));
        return ds[nth].retain;
    };

    { // PrepareRestart 把保留从默认的 300ms 延长：过了默认值通道仍在，回来能恢复。
        ShmChannelClient a = MustConnect(env.ep, "app-p");
        CHECK(a.SendPrepareRestart(1500ms) == 0);
    }
    CHECK(WaitUntil([&] { return env.server->stats().detached_channels == 1; }));
    CHECK(retain_of(0) == 1500ms);
    std::this_thread::sleep_for(700ms); // 默认的 300ms 早过了
    ExpectTrace(env.handler, "app-p", "OD");
    {
        ShmChannelClient b = MustConnect(env.ep, "app-p");
        CHECK(b.resumed());
        // PrepareRestart 只管下一次断开：恢复之后再断，用默认值。
    }
    CHECK(WaitUntil([&] { return env.server->stats().detached_total == 2; }));
    CHECK(retain_of(1) == 300ms);
    CHECK(WaitUntil([&] { return env.handler.closed_reason("app-p") == ShmCloseReason::RetentionExpired; }));

    { // 超过 retain_max 的按上限算。
        ShmChannelClient a = MustConnect(env.ep, "app-q");
        CHECK(a.SendPrepareRestart(600s) == 0);
    }
    CHECK(WaitUntil([&] { return env.server->stats().detached_total == 3; }));
    CHECK(retain_of(2) == 2000ms);

    { // 客户端自己拒绝没有意义的参数；没连上时报 ENOTCONN。
        ShmChannelClient c = MustConnect(env.ep, "app-r2");
        CHECK(c.SendPrepareRestart(0ms) == EINVAL);
        CHECK(c.SendPrepareRestart(-5ms) == EINVAL);
        CHECK(ShmChannelClient{}.SendPrepareRestart(1s) == ENOTCONN);
        CHECK(ShmChannelClient{}.SendBye() == ENOTCONN);
    }
}

void TestBye()
{
    LeakCheck leak;
    Env env(Ret());
    {
        ShmChannelClient a = MustConnect(env.ep, "app-b");
        SendRange(a, 0, 3);
        CHECK(a.SendBye() == 0);
    }
    CHECK(WaitUntil([&] { return env.handler.closed_reason("app-b") == ShmCloseReason::PeerBye; }, 2s));
    CHECK(env.handler.echoed.load() == 3);
    CHECK(env.handler.trace("app-b") == "OC"); // 没有经过 Detached
    const ShmServerStats st = env.server->stats();
    CHECK(st.byes == 1 && st.detached_total == 0 && st.open_channels == 0);
    // 不保留：同名再来是全新的通道。
    ShmChannelClient b = MustConnect(env.ep, "app-b");
    CHECK(!b.resumed() && b.app_epoch() == 1);
    ExpectRxEmpty(b);

    // PrepareRestart 之后又发 Bye：Bye 说了算。
    {
        ShmChannelClient c = MustConnect(env.ep, "app-b2");
        CHECK(c.SendPrepareRestart(20s) == 0);
        CHECK(c.SendBye() == 0);
    }
    CHECK(WaitUntil([&] { return env.handler.closed_reason("app-b2") == ShmCloseReason::PeerBye; }, 2s));
    CHECK(env.handler.trace("app-b2") == "OC");
}

void TestRawControlMessages()
{
    LeakCheck leak;
    Env env(Ret(5s));
    // 不合法的控制消息：协议错误，关闭，不进保留（对端不老实，没必要替它留着）。
    auto bad = [&](const char* busid, const wire::Control& c)
    {
        Raw r = RawAttach(env.ep, busid);
        RawSend(r.fd.get(), &c, sizeof(c));
        CHECK(WaitUntil([&] { return env.handler.closed_reason(busid) == ShmCloseReason::ProtocolError; }));
        CHECK(RawSeesEof(r.fd.get()));
        CHECK(env.handler.trace(busid) == "OC");
    };
    bad("c-zero", wire::MakeControl(wire::ControlOp::kPrepareRestart, 0));
    bad("c-bye-arg", wire::MakeControl(wire::ControlOp::kBye, 7));
    bad("c-op", wire::MakeControl(static_cast<wire::ControlOp>(99), 1));
    {
        wire::Control c = wire::MakeControl(wire::ControlOp::kBye, 0);
        c.hdr.version = wire::kVersion + 1;
        bad("c-version", c);
    }
    CHECK(env.server->stats().protocol_errors == 4);
    CHECK(env.server->stats().detached_total == 0);

    { // 垃圾在第一个不对的字节就识别出来，不用攒够 16 字节。
        Raw r = RawAttach(env.ep, "c-junk");
        RawSend(r.fd.get(), "Z", 1);
        CHECK(WaitUntil([&] { return env.handler.closed_reason("c-junk") == ShmCloseReason::ProtocolError; }));
    }
    { // 控制消息分两段到：照常生效。
        Raw r = RawAttach(env.ep, "c-split");
        const wire::Control c = wire::MakeControl(wire::ControlOp::kPrepareRestart, 3000);
        RawSend(r.fd.get(), &c, 3);
        std::this_thread::sleep_for(30ms);
        RawSend(r.fd.get(), reinterpret_cast<const char*>(&c) + 3, sizeof(c) - 3);
        std::this_thread::sleep_for(30ms);
        r.fd.reset();
        CHECK(WaitUntil([&] { return env.handler.trace("c-split") == "OD"; }));
        bool seen = false;
        for (const auto& e : env.handler.events())
            seen |= e.busid == "c-split" && e.kind == EchoHandler::Event::Kind::Detached &&
                    e.retain == 3000ms;
        CHECK(seen);
    }
    { // 半条控制消息之后就走了：是 App 没了，不是协议错误；半条的内容不生效，用默认保留。
        Raw r = RawAttach(env.ep, "c-half");
        const wire::Control c = wire::MakeControl(wire::ControlOp::kPrepareRestart, 3000);
        RawSend(r.fd.get(), &c, 8);
        r.fd.reset();
        CHECK(WaitUntil([&] { return env.handler.trace("c-half") == "OD"; }));
        for (const auto& e : env.handler.events())
        {
            if (e.busid == "c-half" && e.kind == EchoHandler::Event::Kind::Detached)
                CHECK(e.retain == 5s);
        }
    }
    CHECK(env.server->stats().protocol_errors == 5);
}

// App 在读到 Accept 之前就被 kill：Accept 还躺在它的接收队列里，内核关闭它的 socket 时给服务端这一端记 ECONNRESET
// （而不是 EOF）。这同样是"App 没了"：通道要进保留，而不是当 IO 故障销毁。
void TestAppKilledBeforeReadingAccept()
{
    LeakCheck leak;
    Env env(Ret());
    {
        UniqueFd fd = RawConnect(env.ep);
        const wire::Hello h = MakeHello("app-reset");
        RawSend(fd.get(), &h, sizeof(h));
        CHECK(WaitUntil([&] { return env.handler.opened_count() == 1; })); // 服务端已经发出 Accept
        // 不读 Accept，直接关。
    }
    ExpectTrace(env.handler, "app-reset", "OD");
    CHECK(env.server->stats().detached_channels == 1);
    ShmChannelClient b = MustConnect(env.ep, "app-reset");
    CHECK(b.resumed());
}

// ---- 恢复的规矩 ----

void TestResumeRules()
{
    LeakCheck leak;
    Env env(Ret());

    { // 活着的同名通道：EBUSY，不抢。busy_retry 窗口内会重试，窗口过了才返回。
        ShmChannelClient a = MustConnect(env.ep, "dup");
        ShmClientOptions o = ClientOpts(env.ep, "dup");
        o.busy_retry = 0ms;
        const auto t_fast = std::chrono::steady_clock::now();
        auto r = ShmChannelClient::Connect(o);
        CHECK(!r.has_value() && r.error() == EBUSY);
        CHECK(std::chrono::steady_clock::now() - t_fast < 1s); // 不重试：立即返回，不是耗到 timeout
        o.busy_retry = 300ms;
        const auto t0 = std::chrono::steady_clock::now();
        r = ShmChannelClient::Connect(o);
        CHECK(!r.has_value() && r.error() == EBUSY);
        CHECK(std::chrono::steady_clock::now() - t0 >= 250ms); // 确实重试到窗口结束
        CHECK(std::chrono::steady_clock::now() - t0 < 2s);     // 也只到窗口结束（timeout 是 5 秒）
        EchoRoundTrips(a, 5);                                 // 失败的尝试没有碰原来的通道
    }
    CHECK(WaitUntil([&] { return env.server->stats().detached_channels == 1; }));

    { // 重试窗口里对方释放了：后一次尝试恢复成功。
        ShmClientOptions o = ClientOpts(env.ep, "dup");
        o.busy_retry = 3s;
        auto a = ShmChannelClient::Connect(o);
        CHECK(a.has_value() && a->resumed());
        std::thread releaser(
            [&]
            {
                std::this_thread::sleep_for(150ms);
                *a = ShmChannelClient{}; // 放掉（关 UDS）
            });
        auto b = ShmChannelClient::Connect(o);
        releaser.join();
        CHECK(b.has_value() && b->resumed() && b->app_epoch() == 3);
    }

    { // 容量对不上：沿用原容量，不一致 EINVAL；0 表示不指定；被拒绝不毁掉保留着的队列。
        const std::uint64_t base = env.handler.echoed.load();
        {
            ShmChannelClient a = MustConnect(env.ep, "cap");
            SendRange(a, 0, 2);
        }
        CHECK(WaitUntil([&] { return env.handler.echoed.load() >= base + 2; }));
        CHECK(ConnectErr(env.ep, "cap", 2 * shm::kMinRingCapacity) == EINVAL);
        {
            ShmClientOptions o = ClientOpts(env.ep, "cap");
            o.tx_max_payload = 12345;
            auto r = ShmChannelClient::Connect(o);
            CHECK(!r.has_value() && r.error() == EINVAL);
        }
        ShmClientOptions o = ClientOpts(env.ep, "cap");
        o.tx_capacity = 0; // 恢复时 0 = 不指定，沿用
        o.rx_capacity = 0;
        auto b = ShmChannelClient::Connect(o);
        CHECK(b.has_value() && b->resumed());
        CHECK(b->shm().layout().tx.capacity == shm::kMinRingCapacity);
        ExpectRx(*b, {0, 1}); // 队列还在
    }
}

void TestDiscardRetained()
{
    LeakCheck leak;
    Env env(Ret());
    std::uint64_t old_id = 0;
    {
        ShmChannelClient a = MustConnect(env.ep, "app-dr");
        CHECK(env.AgentPush("app-dr", 7, LenOf(7)));
        old_id = env.handler.events().front().id;
    }
    CHECK(WaitUntil([&] { return env.server->stats().detached_channels == 1; }));

    { // 容量不合法：失败，旧队列没被毁掉。
        ShmClientOptions o = ClientOpts(env.ep, "app-dr");
        o.discard_retained = true;
        o.tx_capacity = 3 * shm::kMinRingCapacity;
        auto r = ShmChannelClient::Connect(o);
        CHECK(!r.has_value() && r.error() == EINVAL);
        CHECK(env.server->stats().detached_channels == 1 && env.server->stats().discarded == 0);
    }
    { // 不带 discard 却要不同的容量：EINVAL（上面的用例已覆盖）；带了就按新容量重建。
        ShmClientOptions o = ClientOpts(env.ep, "app-dr");
        o.discard_retained = true;
        o.rx_capacity = 2 * shm::kMinRingCapacity;
        auto b = ShmChannelClient::Connect(o);
        CHECK(b.has_value());
        CHECK(!b->resumed() && b->app_epoch() == 1);
        CHECK(b->shm().layout().rx.capacity == 2 * shm::kMinRingCapacity);
        ExpectRxEmpty(*b); // 旧队列丢了
        EchoRoundTrips(*b, 10);
        // 新通道先于旧通道的 OnClosed 出现：O D O C，id 不同，旧的原因是 Discarded。
        CHECK(WaitUntil([&] { return env.handler.trace("app-dr") == "ODOC"; }));
        const auto ev = env.handler.events();
        CHECK(ev[0].id == old_id && ev[1].id == old_id);
        CHECK(ev[2].id != old_id && ev[2].kind == EchoHandler::Event::Kind::Opened);
        CHECK(ev[3].id == old_id && ev[3].reason == ShmCloseReason::Discarded);
        CHECK(env.server->stats().discarded == 1 && env.server->stats().open_channels == 1);
        // 新通道登记在这个名字下（没被旧通道延后的拆除顶掉）：同名再来要 EBUSY，而不是又建出一条。
        ShmClientOptions dup = ClientOpts(env.ep, "app-dr");
        dup.busy_retry = 0ms;
        auto again = ShmChannelClient::Connect(dup);
        CHECK(!again.has_value() && again.error() == EBUSY);
        CHECK(env.server->stats().open_channels == 1);
    }
    { // 没有保留着的东西时，discard 标志无害：就是一次普通的新建。
        ShmClientOptions o = ClientOpts(env.ep, "app-fresh");
        o.discard_retained = true;
        auto c = ShmChannelClient::Connect(o);
        CHECK(c.has_value() && !c->resumed());
        CHECK(env.server->stats().discarded == 1);
    }
}

// 配额只够一条通道时，discard_retained 重建要成功：丢掉的旧通道占的名额与内存在判断时抵扣（否则永远 ENOSPC）。
void TestDiscardUnderTightQuota()
{
    LeakCheck leak;
    ShmServerOptions o = Ret();
    o.max_channels = 1;
    o.max_total_bytes = 3ULL << 20; // 每条通道约 2 MiB + 12 KiB：只放得下一条
    Env env(o);
    {
        ShmChannelClient a = MustConnect(env.ep, "tight");
    }
    CHECK(WaitUntil([&] { return env.server->stats().detached_channels == 1; }));
    // 不带 discard 又要不同的容量：EINVAL（不是 ENOSPC：沿用原容量不占新的配额）。
    CHECK(ConnectErr(env.ep, "tight", 2 * shm::kMinRingCapacity) == EINVAL);
    // 另一个名字：配额满了。
    CHECK(ConnectErr(env.ep, "other") == ENOSPC);
    // 带 discard 重建同一个名字：成功。
    ShmClientOptions d = ClientOpts(env.ep, "tight");
    d.discard_retained = true;
    auto b = ShmChannelClient::Connect(d);
    CHECK(b.has_value() && !b->resumed());
    CHECK(WaitUntil([&] { return env.server->stats().discarded == 1 && env.server->stats().open_channels == 1; }));
    EchoRoundTrips(*b, 5);
}

void TestResumeIdentity()
{
    if (::geteuid() != 0)
    {
        std::puts("TestResumeIdentity: 跳过（要 root 才能换用户）");
        return;
    }
    LeakCheck leak;
    Env env(Ret());
    OpenSocketToAll(env);
    {
        ShmChannelClient a = MustConnect(env.ep, "app-u");
        SendRange(a, 0, 2);
    }
    CHECK(WaitUntil([&] { return env.handler.echoed.load() == 2; }));
    const std::size_t admits = env.handler.admits().size();

    // 另一个用户想接手保留着的队列：EACCES，而且在准入回调之前就拒绝了。
    const int e = AsUser(65534,
                         [&]
                         {
                             ShmClientOptions o = ClientOpts(env.ep, "app-u");
                             o.busy_retry = 0ms;
                             auto r = ShmChannelClient::Connect(o);
                             return r.has_value() ? 0 : r.error();
                         });
    CHECK(e == EACCES);
    CHECK(env.handler.admits().size() == admits);
    CHECK(env.server->stats().detached_channels == 1); // 保留着的队列没动

    // 别的用户来全新的通道没问题；只是不能碰别人的保留。
    const int e2 = AsUser(65534,
                          [&]
                          {
                              auto r = ShmChannelClient::Connect(ClientOpts(env.ep, "app-u2"));
                              return r.has_value() ? 0 : r.error();
                          });
    CHECK(e2 == 0);

    // 原来的用户回来：队列完好。
    ShmChannelClient b = MustConnect(env.ep, "app-u");
    CHECK(b.resumed());
    ExpectRx(b, {0, 1});

    // Reattach 命中还留着的通道：别的用户拿着同一块内存的 fd 来也不行（fd 在进程里是共享的，换用户只是换了线程的凭据）。
    ShmChannelClient c = MustConnect(env.ep, "app-u3");
    CHECK(::shutdown(c.liveness_fd(), SHUT_RDWR) == 0);
    CHECK(WaitUntil([&] { return env.server->stats().detached_channels == 2; })); // app-u2 与 app-u3
    ShmClientOptions once = ClientOpts(env.ep, "app-u3");
    once.busy_retry = 0ms;
    const int e3 = AsUser(65534, [&] { return c.Reattach(once); });
    CHECK(e3 == EACCES);
    CHECK(env.server->stats().detached_channels == 2 && env.server->stats().reattached == 0);
    CHECK(c.Reattach(once) == 0); // 本人可以
}

void TestRapidRestartNeverSeesBusy()
{
    LeakCheck leak;
    Env env(Ret());
    ShmChannelClient cur = MustConnect(env.ep, "app-flap");
    constexpr int kRounds = 200;
    for (int i = 0; i < kRounds; ++i)
    {
        cur = ShmChannelClient{}; // 旧进程退出：UDS 的 EOF 已经在内核里排着，Agent 的事件循环可能还没处理
        ShmClientOptions o = ClientOpts(env.ep, "app-flap");
        o.busy_retry = 0ms; // 不靠客户端重试：服务端见到同名 Hello 要自己确认旧连接是不是已经断了
        auto next = ShmChannelClient::Connect(o);
        if (!next.has_value())
            std::fprintf(stderr, "第 %d 轮 Connect 失败 errno=%d\n", i, next.error());
        CHECK(next.has_value());
        CHECK(next->resumed() && next->app_epoch() == static_cast<std::uint64_t>(i) + 2);
        cur = std::move(*next);
    }
    EchoRoundTrips(cur, 5);
    CHECK(WaitUntil([&] { return env.server->stats().resumed == kRounds; }));
    const ShmServerStats st = env.server->stats();
    CHECK(st.opened == 1 && st.rejected == 0);
}

// IO 线程的事件按就绪的先后处理：旧连接的 EOF 通常先于新连接的 Hello 被处理，Hello 来的时候旧通道已经在保留中。
// 只有"新进程先连上（会话已建立）、旧进程随后才退出"（滚动更新时新旧重叠）时，Hello 会排在 EOF 前面：
// 此时旧通道在 Agent 眼里还是在线的，要靠服务端见到同名 Hello 时先探一下旧连接（ProbeAlive）才能当场恢复。
// 造法：让 IO 线程卡在一个回调里，卡住期间先发新 Hello、再关旧连接，放开之后两个事件按这个顺序就绪。
void TestHelloBeforeOldEofStillResumes()
{
    LeakCheck leak;
    Env env(Ret());
    std::mutex mu;
    std::condition_variable cv;
    bool entered = false, gate_open = true, armed = false;
    env.handler.readable_hook = [&]
    {
        std::unique_lock lk(mu);
        if (!armed)
            return;
        armed = false;
        entered = true;
        cv.notify_all();
        cv.wait(lk, [&] { return gate_open; });
    };
    const auto block_loop = [&](auto&& during)
    {
        { // 武装：下一次 OnReadable 会卡住
            std::lock_guard lk(mu);
            armed = true;
            entered = false;
            gate_open = false;
        }
        during(); // 触发那次 OnReadable
        {
            std::unique_lock lk(mu);
            CHECK(cv.wait_for(lk, 5s, [&] { return entered; }));
        }
    };
    const auto release_loop = [&]
    {
        std::lock_guard lk(mu);
        gate_open = true;
        cv.notify_all();
    };

    { // Attach：新连接先连上（会话建立、等 Hello），旧的随后才关。
        ShmChannelClient a = MustConnect(env.ep, "app-race");
        UniqueFd b = RawConnect(env.ep);
        std::this_thread::sleep_for(100ms); // 让服务端 accept 并登记这条连接
        block_loop([&] { CHECK(SendMsg(a, 0, LenOf(0))); });
        const wire::Hello h = MakeHello("app-race");
        RawSend(b.get(), &h, sizeof(h)); // Hello 先就绪
        a = ShmChannelClient{};          // 旧连接的 EOF 后就绪
        release_loop();
        auto r = RawRecvReply(b.get());
        CHECK(r.has_value() && r->reply.result == 0 && r->reply.resumed == 1 && r->nfds == 3);
    }
    { // Reattach 命中还留着的通道：同样的次序。
        CHECK(WaitUntil([&] { return env.server->stats().open_channels == 1; }));
        // 先把上面那条恢复出来的通道断掉，换一个干净的通道名做 Reattach。
        ShmChannelClient a = MustConnect(env.ep, "app-race2");
        const auto fds = FdsOf(a.shm());
        UniqueFd b = RawConnect(env.ep);
        std::this_thread::sleep_for(100ms);
        block_loop([&] { CHECK(SendMsg(a, 0, LenOf(0))); });
        wire::Hello h = MakeHello("app-race2");
        h.attach_type = static_cast<std::uint32_t>(wire::AttachType::kReattach);
        h.tx_capacity = 0;
        h.rx_capacity = 0;
        CHECK(sock::SendWithFds(b.get(), std::as_bytes(std::span(&h, 1)), fds) == 0);
        CHECK(::shutdown(a.liveness_fd(), SHUT_RDWR) == 0); // 旧连接断开，但进程（内存）还在
        release_loop();
        auto r = RawRecvReply(b.get());
        CHECK(r.has_value() && r->reply.result == 0 && r->reply.resumed == 1 && r->nfds == 0);
    }
    env.handler.readable_hook = nullptr;
}

void TestServerStopDestroysRetained()
{
    LeakCheck leak;
    Env env(Ret());
    {
        ShmChannelClient a = MustConnect(env.ep, "app-s1");
        ShmChannelClient b = MustConnect(env.ep, "app-s2");
    }
    CHECK(WaitUntil([&] { return env.server->stats().detached_channels == 2; }));
    env.server->Stop();
    CHECK(WaitUntil([&] { return env.handler.closed_count() == 2; }));
    CHECK(env.handler.closed_reason("app-s1") == ShmCloseReason::ServerStopped);
    CHECK(env.handler.closed_reason("app-s2") == ShmCloseReason::ServerStopped);
    CHECK(env.server->stats().detached_channels == 0 && env.server->stats().open_channels == 0);
}

void TestHandlerClosesDetachedChannel()
{
    LeakCheck leak;
    Env env(Ret());
    {
        ShmChannelClient a = MustConnect(env.ep, "app-hc");
    }
    CHECK(WaitUntil([&] { return env.server->stats().detached_channels == 1; }));
    // 业务决定不要这条保留的通道（比如路由层发现它已经下线）：Close 立刻销毁，保留计时器随之取消。
    env.OnLoop(
        [&]
        {
            ShmServerChannel* c = env.handler.FindLive("app-hc");
            CHECK(c != nullptr && c->detached());
            c->Close();
            return 0;
        });
    CHECK(WaitUntil([&] { return env.handler.closed_reason("app-hc") == ShmCloseReason::LocalClose; }));
    CHECK(env.server->stats().detached_channels == 0);
    ShmChannelClient b = MustConnect(env.ep, "app-hc");
    CHECK(!b.resumed());
}

// ---- Agent 重启与 Reattach ----

void TestReattachAfterAgentRestart()
{
    LeakCheck leak;
    TempDir dir;
    const Endpoint ep = dir.Unix("agent.sock");
    const ShmClientOptions opts = ClientOpts(ep, "app-ra");
    auto agent1 = std::make_unique<Env>(Ret(), ep);
    ShmChannelClient a = MustConnect(ep, "app-ra");
    CHECK(a.app_epoch() == 1 && a.agent_epoch() == 1 && !a.resumed());
    EchoRoundTrips(a, 5);
    // 旧 Agent 在世时往 Rx 写了 4 条，App 还没读。
    for (std::uint64_t i = 500; i < 504; ++i)
        CHECK(agent1->AgentPush("app-ra", i, LenOf(i)));

    agent1.reset(); // Agent 没了：通道销毁、UDS 关闭；App 手里的映射与 fd 还在
    CHECK(WaitUntil([&] { return a.AgentGone(); }));
    CHECK(!a.Superseded());

    // Agent 不在期间 App 的 Tx 照常写。
    SendRange(a, 600, 6);
    // 模拟旧 Agent 死在处理第 600 条的期间（inflight 标记等于 read_idx），并且睡着时死的（睡眠标志停在 1）。
    a.shm().tx_control().inflight_idx.store(a.shm().tx_control().read_idx.load());
    a.shm().tx_control().consumer_sleeping.store(1);

    Env agent2(Ret(), ep);
    CHECK(a.Reattach(opts) == 0);
    CHECK(a.resumed() && a.app_epoch() == 1 && a.agent_epoch() == 2 && !a.Superseded());
    CHECK(!a.AgentGone()); // 连接已经换成通向新 Agent 的那条
    // Rx：旧 Agent 写的 4 条先读到；新 Agent 处理 601..605 并回显，600 被跳过（至多一次）。
    ExpectRx(a, {500, 501, 502, 503, 601, 602, 603, 604, 605});
    ExpectRxEmpty(a);
    CHECK(WaitUntil([&] { return agent2.handler.echoed.load() == 5; })); // 不是 6：被标记的那一条跳过了
    CHECK(WaitUntil([&] { return agent2.handler.reattached_opens() == 1; }));
    CHECK(WaitUntil([&] { return agent2.server->stats().reattached == 1; }));
    const ShmServerStats st = agent2.server->stats();
    CHECK(st.inflight_skipped == 1 && st.resumed == 0 && st.opened == 1);
    ExpectTrace(agent2.handler, "app-ra", "O");
    // 之后一切照常（睡眠 / 唤醒协议在新 Agent 上重新建立）。
    EchoRoundTrips(a, 30, 1000);
    EchoRoundTrips(a, 30, 2000);
}

void TestReattachWhileAgentStillHasChannel()
{
    LeakCheck leak;
    Env env(Ret());
    const ShmClientOptions opts = ClientOpts(env.ep, "app-h");
    ShmChannelClient a = MustConnect(env.ep, "app-h");
    EchoRoundTrips(a, 3);
    // App 这边的 UDS 断了（进程没死）：Agent 看到 EOF，通道进入保留。
    CHECK(::shutdown(a.liveness_fd(), SHUT_RDWR) == 0);
    CHECK(WaitUntil([&] { return env.server->stats().detached_channels == 1; }));
    CHECK(a.AgentGone());
    // Reattach：Agent 认出交来的是同一块内存，原样恢复；还是原来的 App，epoch 不变，也不会再发 fd。
    CHECK(a.Reattach(opts) == 0);
    CHECK(a.resumed() && a.app_epoch() == 1 && a.agent_epoch() == 1 && !a.Superseded());
    CHECK(!a.AgentGone()); // 换了新的 UDS 连接（旧的已经被我们自己 shutdown 了）
    ExpectTrace(env.handler, "app-h", "ODR");
    CHECK(WaitUntil([&] { return env.server->stats().reattached == 1; }));
    const ShmServerStats st = env.server->stats();
    CHECK(st.resumed == 0 && st.opened == 1 && st.inflight_skipped == 0);
    EchoRoundTrips(a, 10, 100);

    // 再断一次，这回由别的进程用 Attach 接手：本进程的 app_epoch 过期，应当退出。
    CHECK(::shutdown(a.liveness_fd(), SHUT_RDWR) == 0);
    CHECK(WaitUntil([&] { return env.server->stats().detached_channels == 1; }));
    std::optional<ShmChannelClient> b = ShmChannelClient::Connect(ClientOpts(env.ep, "app-h")).value();
    CHECK(b->resumed() && b->app_epoch() == 2);
    CHECK(a.Superseded());
    ShmClientOptions once = opts;
    once.busy_retry = 0ms;
    CHECK(a.Reattach(once) == EBUSY); // 新进程活着，不让抢
    b.reset();
    CHECK(WaitUntil([&] { return env.server->stats().detached_channels == 1; }));
    CHECK(a.Reattach(once) == ESTALE); // 通道还留着，但 epoch 对不上：Agent 的回复里 app_epoch 是 2
    CHECK(a.agent_epoch() == 1);       // 失败时本对象没被改动
}

// 假 App 带着一块自己造的内存来 Reattach。
int RawReattach(const Endpoint& ep, std::string_view busid, std::span<const int> fds, RawReply* out = nullptr)
{
    wire::Hello h = MakeHello(busid);
    h.attach_type = static_cast<std::uint32_t>(wire::AttachType::kReattach);
    h.tx_capacity = 0;
    h.rx_capacity = 0;
    UniqueFd fd = RawConnect(ep);
    CHECK(sock::SendWithFds(fd.get(), std::as_bytes(std::span(&h, 1)), fds) == 0);
    auto r = RawRecvReply(fd.get());
    CHECK(r.has_value() && wire::CheckReplyHeader(r->reply) == 0);
    const int result = r->reply.result;
    if (result != 0)
        CHECK(r->nfds == 0);
    if (out != nullptr)
        *out = std::move(*r);
    return result;
}

void TestReattachHostileInput()
{
    LeakCheck leak;
    ShmServerOptions o = Ret();
    o.max_ring_capacity = shm::kMinRingCapacity;
    o.max_channels = 3;
    Env env(o);
    const auto no_new_channels = [&] { return env.handler.opened_count() == 0; };

    { // fd 不是 memfd。
        shm::ShmChannel ch = NewShm();
        int p[2];
        CHECK(::pipe2(p, O_CLOEXEC) == 0);
        UniqueFd p0(p[0]), p1(p[1]);
        const std::array<int, 3> junk{p[0], ch.tx_eventfd(), ch.rx_eventfd()};
        CHECK(RawReattach(env.ep, "h1", junk) == EINVAL);
    }
    { // memfd 没有封印：对端能 ftruncate，Agent 一访问被截掉的页就 SIGBUS。
        auto mem = shm::CreateMemFd("unsealed", 1 << 22);
        auto e1 = shm::CreateEventFd();
        auto e2 = shm::CreateEventFd();
        CHECK(mem.has_value() && e1.has_value() && e2.has_value());
        const std::array<int, 3> fds{mem->get(), e1->get(), e2->get()};
        CHECK(RawReattach(env.ep, "h2", fds) == EINVAL);
    }
    { // eventfd 的位置放了别的东西（阻塞的管道会让 Agent 卡住）。
        shm::ShmChannel ch = NewShm();
        int p[2];
        CHECK(::pipe2(p, O_CLOEXEC) == 0);
        UniqueFd p0(p[0]), p1(p[1]);
        const std::array<int, 3> fds{ch.memfd(), p[1], ch.rx_eventfd()};
        CHECK(RawReattach(env.ep, "h3", fds) == EINVAL);
    }
    { // 头里的魔数被改：布局不认识。
        shm::ShmChannel ch = NewShm();
        ch.header().fixed.magic ^= 1;
        CHECK(RawReattach(env.ep, "h4", FdsOf(ch)) == EPROTO);
    }
    { // 游标越界：占用超过容量。
        shm::ShmChannel ch = NewShm();
        ch.tx_control().write_idx.store(ch.layout().tx.capacity + 4096);
        CHECK(RawReattach(env.ep, "h5", FdsOf(ch)) == EINVAL);
    }
    { // 容量超过 Agent 的单环上限。
        shm::ShmChannel ch = NewShm(2 * shm::kMinRingCapacity);
        CHECK(RawReattach(env.ep, "h6", FdsOf(ch)) == EINVAL);
    }
    { // fd 数量不对：0、1、2、4 个都是 EINVAL。
        shm::ShmChannel ch = NewShm();
        const auto all = FdsOf(ch);
        CHECK(RawReattach(env.ep, "h7", std::span<const int>(all).first(0)) == EINVAL);
        CHECK(RawReattach(env.ep, "h7", std::span<const int>(all).first(1)) == EINVAL);
        CHECK(RawReattach(env.ep, "h7", std::span<const int>(all).first(2)) == EINVAL);
        const std::array<int, 4> four{all[0], all[1], all[2], all[0]};
        CHECK(RawReattach(env.ep, "h7", four) == EINVAL);
    }
    CHECK(no_new_channels());
    CHECK(env.server->stats().rejected == 10); // h1..h6 各一次，h7 四次

    { // inflight 标记比 read_idx 还大：合法状态下不可能出现，按页被写坏处理，拒绝（不尝试修复）。
        shm::ShmChannel ch = NewShm();
        shm::RingProducer tx = ch.TxProducer();
        auto r = tx.TryReserve(LenOf(0));
        CHECK(r.status == shm::ReserveStatus::kOk);
        Fill(r.payload, 0);
        tx.Commit();
        ch.tx_control().inflight_idx.store(ch.tx_control().write_idx.load()); // > read_idx（0），且不超过 write_idx
        CHECK(RawReattach(env.ep, "h11", FdsOf(ch)) == EINVAL);
    }
    { // inflight 指着的那条记录的头被写坏：不能按它给的长度去跳。
        shm::ShmChannel ch = NewShm();
        shm::RingProducer tx = ch.TxProducer();
        auto r = tx.TryReserve(LenOf(0));
        CHECK(r.status == shm::ReserveStatus::kOk);
        Fill(r.payload, 0);
        const std::uint32_t bad_len = 0x7FFFFFFF;
        std::memcpy(r.payload.data() - shm::kRecordHeaderSize, &bad_len, sizeof(bad_len)); // 记录头的 len 字段
        tx.Commit();
        ch.tx_control().inflight_idx.store(0); // == read_idx
        CHECK(RawReattach(env.ep, "h12", FdsOf(ch)) == EINVAL);
    }
    CHECK(no_new_channels());

    { // 准入回调拒绝：errno 原样回，回调里看得到这是 Reattach。
        shm::ShmChannel ch = NewShm();
        env.handler.admit_deny.store(EACCES);
        CHECK(RawReattach(env.ep, "h8", FdsOf(ch)) == EACCES);
        env.handler.admit_deny.store(0);
        const auto admits = env.handler.admits();
        CHECK(admits.back().busid == "h8" && admits.back().reattach && !admits.back().resuming);
    }
    { // 通道名被活着的、另一块内存的通道占着：EBUSY。
        ShmChannelClient live = MustConnect(env.ep, "h9");
        shm::ShmChannel ch = NewShm();
        CHECK(RawReattach(env.ep, "h9", FdsOf(ch)) == EBUSY);
        EchoRoundTrips(live, 3); // 活着的那条没受影响
    }
    CHECK(WaitUntil([&] { return env.server->stats().detached_channels == 1; })); // h9 的 App 走了，进入保留
    { // 通道名被保留着的通道占着：交来的不是同一块内存，EBUSY；原来的 App 仍然能恢复。
        shm::ShmChannel ch = NewShm();
        CHECK(RawReattach(env.ep, "h9", FdsOf(ch)) == EBUSY);
        auto back = ShmChannelClient::Connect(ClientOpts(env.ep, "h9"));
        CHECK(back.has_value() && back->resumed());
    }
    { // 通道数配额（max_channels = 3）：已经有 h9；再占两条就满了。
        ShmChannelClient c1 = MustConnect(env.ep, "q1");
        ShmChannelClient c2 = MustConnect(env.ep, "q2");
        shm::ShmChannel ch = NewShm();
        CHECK(RawReattach(env.ep, "h10", FdsOf(ch)) == ENOSPC);
    }

    // 一个规矩的 Reattach 被接受：服务端随后真的在这块内存上工作。
    Env clean(Ret());
    shm::ShmChannel ch = NewShm();
    shm::RingProducer tx = ch.TxProducer();
    shm::RingConsumer rx = ch.RxConsumer();
    for (std::uint64_t i = 0; i < 3; ++i)
    {
        auto r = tx.TryReserve(LenOf(i));
        CHECK(r.status == shm::ReserveStatus::kOk);
        Fill(r.payload, i);
        tx.Commit();
    }
    RawReply reply;
    CHECK(RawReattach(clean.ep, "good", FdsOf(ch), &reply) == 0);
    CHECK(reply.reply.resumed == 1 && reply.nfds == 0 && reply.reply.agent_epoch == 1 && reply.reply.app_epoch == 0);
    CHECK(WaitUntil([&] { return clean.handler.reattached_opens() == 1; }));
    const auto drain = [&](std::uint64_t first, std::uint64_t count)
    {
        for (std::uint64_t want = first; want < first + count; ++want)
        {
            shm::RecordView v;
            CHECK(WaitUntil([&] { return rx.Peek(v) == shm::PeekStatus::kOk; }));
            std::uint64_t seq = 0;
            CHECK(Verify(v.payload, v.len, &seq) && seq == want);
            rx.Pop();
        }
    };
    drain(0, 3);
    // 恢复之后的唤醒协议在这块内存上也正常：Agent 睡着了，App 一发就能叫醒它。
    for (std::uint64_t i = 10; i < 15; ++i)
    {
        auto r = tx.TryReserve(LenOf(i));
        CHECK(r.status == shm::ReserveStatus::kOk);
        Fill(r.payload, i);
        tx.Commit();
    }
    drain(10, 5);
}

// Reattach 交来的内存也要过共享内存总量的配额（App 可以交一块很大的内存来占 Agent 的配额）。
void TestReattachTotalQuota()
{
    LeakCheck leak;
    ShmServerOptions o = Ret();
    o.max_total_bytes = 3ULL << 20; // 每条通道约 2 MiB + 12 KiB：放得下一条
    Env env(o);
    ShmChannelClient live = MustConnect(env.ep, "tq-live");
    shm::ShmChannel ch = NewShm();
    CHECK(RawReattach(env.ep, "tq-new", FdsOf(ch)) == ENOSPC);
    EchoRoundTrips(live, 3); // 占着配额的那条不受影响
}

// 假 Agent 对 Reattach 回错的东西：客户端各自怎么反应。
void TestClientReattachAgainstHostileAgent()
{
    LeakCheck leak;
    auto first = [](int conn, const wire::Hello&)
    {
        shm::ShmChannel ch = NewShm();
        // 共享内存头里的 epoch 要与回复里说的一致（GoodReply 说的是 1），否则客户端会认为通道已被别人接管。
        (void)ch.BumpAgentEpoch();
        (void)ch.BumpAppEpoch();
        const auto fds = FdsOf(ch);
        SendReply(conn, GoodReply(ch), fds);
    };
    // 回复里该有的值：布局与 epoch 对得上；各用例在此基础上改一处。
    auto reply_with = [](auto mutate, std::size_t nfds = 0)
    {
        return [mutate, nfds](int conn, const wire::Hello& h)
        {
            CHECK(h.attach_type == static_cast<std::uint32_t>(wire::AttachType::kReattach));
            shm::ShmChannel ch = NewShm(); // 布局与客户端手里的相同
            wire::Reply r = GoodReply(ch);
            r.resumed = 1;
            mutate(r);
            std::vector<int> fds;
            for (std::size_t i = 0; i < nfds; ++i)
                fds.push_back(i == 0 ? ch.memfd() : (i == 1 ? ch.tx_eventfd() : ch.rx_eventfd()));
            SendReply(conn, r, fds);
        };
    };
    // 返回 Reattach 的结果；agent_epoch_out 取回之后客户端记着的 agent_epoch。
    auto run = [&](std::vector<FakeAgent::Behavior> behaviors,
                   std::uint64_t* agent_epoch_out = nullptr,
                   const std::function<void(ShmChannelClient&)>& before = nullptr)
    {
        TempDir dir;
        const Endpoint ep = dir.Unix("fake.sock");
        behaviors.insert(behaviors.begin(), first);
        FakeAgent fake(ep, std::move(behaviors));
        ShmClientOptions o = ClientOpts(ep, "fake-app");
        o.busy_retry = 0ms;
        auto c = ShmChannelClient::Connect(o);
        CHECK(c.has_value());
        if (before)
            before(*c);
        const int e = c->Reattach(o);
        if (agent_epoch_out != nullptr)
            *agent_epoch_out = c->agent_epoch();
        c = std::unexpected(0);
        return e;
    };

    std::uint64_t ep = 0;
    CHECK(run({reply_with([](wire::Reply&) {})}, &ep) == 0 && ep == 1);
    // 回复说 app_epoch 没变，但共享内存头里的已经不是本进程拿到的那个值：通道被接管了，不能当作恢复成功。
    CHECK(run({reply_with([](wire::Reply&) {})},
              &ep,
              [](ShmChannelClient& c) { c.shm().header().app_epoch.store(5); }) == ESTALE &&
          ep == 1);
    CHECK(run({reply_with([](wire::Reply& r) { r.agent_epoch = 7; })}, &ep) == 0 && ep == 7); // 新 Agent 的 epoch 更大
    CHECK(run({reply_with([](wire::Reply& r) { r.agent_epoch = 0; })}, &ep) == EPROTO && ep == 1); // 倒退
    CHECK(run({reply_with([](wire::Reply& r) { r.app_epoch = 2; })}, &ep) == ESTALE && ep == 1);   // 被接管
    CHECK(run({reply_with([](wire::Reply& r) { r.resumed = 0; })}) == EPROTO);
    CHECK(run({reply_with([](wire::Reply&) {}, 3)}) == EPROTO); // Reattach 的 Accept 不该带 fd
    CHECK(run({reply_with([](wire::Reply&) {}, 1)}) == EPROTO);
    CHECK(run({reply_with([](wire::Reply& r) { r.transport = static_cast<std::uint32_t>(wire::ChosenTransport::kUds); })}) ==
          EPROTO);
    CHECK(run({reply_with([](wire::Reply& r) { r.total_size += 4096; })}) == EPROTO);
    CHECK(run({reply_with([](wire::Reply& r) { r.rx.capacity *= 2; })}) == EPROTO);
    CHECK(run({reply_with([](wire::Reply& r) { r.reserved = 1; })}) == EPROTO);
    CHECK(run({reply_with([](wire::Reply& r) { r.hdr.magic = 0; })}) == EPROTO);
    CHECK(run({reply_with([](wire::Reply& r) { r.result = EINVAL; })}) == EINVAL); // Reject 的原因原样返回
    CHECK(run({reply_with([](wire::Reply& r) { r.result = ENOSPC; })}) == ENOSPC);

    { // EBUSY 在窗口内重试：第一次 EBUSY，第二次成功。
        TempDir dir;
        const Endpoint ep2 = dir.Unix("fake.sock");
        FakeAgent fake(ep2,
                       std::vector<FakeAgent::Behavior>{first,
                                                        reply_with([](wire::Reply& r) { r.result = EBUSY; }),
                                                        reply_with([](wire::Reply&) {})});
        ShmClientOptions o = ClientOpts(ep2, "fake-app");
        o.busy_retry = 2s;
        auto c = ShmChannelClient::Connect(o);
        CHECK(c.has_value());
        CHECK(c->Reattach(o) == 0);
        CHECK(fake.hellos().size() == 3);
    }
    { // 没有通道可恢复 / 端点不是 UDS / 通道名不合法。
        ShmClientOptions o = ClientOpts(*Endpoint::Unix("/nonexistent/x.sock"), "x");
        CHECK(ShmChannelClient{}.Reattach(o) == EINVAL);
        TempDir dir;
        const Endpoint ep3 = dir.Unix("fake.sock");
        FakeAgent fake(ep3, first);
        ShmClientOptions o2 = ClientOpts(ep3, "fake-app");
        auto c = ShmChannelClient::Connect(o2);
        CHECK(c.has_value());
        ShmClientOptions tcp = o2;
        tcp.endpoint = *Endpoint::Tcp("127.0.0.1", 1);
        CHECK(c->Reattach(tcp) == EPROTONOSUPPORT);
        ShmClientOptions bad = o2;
        bad.busid = "bad name";
        CHECK(c->Reattach(bad) == EINVAL);
    }
}

// ---- 跨进程 ----

// 进程 A：连上，写 100 条（不读回显），不做任何清理就被 SIGKILL。
// 进程 B：同名连上，必须是恢复，读到那 100 条的回显，再正常收发。服务端在本进程。
// 另外：被杀的 Agent 进程由新 Agent 进程接替，App 在本进程里 Reattach；
// 回显只允许丢"被杀那一刻处理到一半的那一条"，不重复、不乱序。
constexpr int kAgentKills = 5;

struct XprocChildren
{
    Child app_a, app_b;
    std::array<Child, kAgentKills + 1> agents;
};

int AgentChild(const Endpoint& ep)
{
    EventLoopGroup group(EventLoopGroupOptions{1, 64});
    if (group.Start() != 0)
        return 31;
    EchoHandler handler;
    handler.use_inflight = true;
    handler.window_spin_us = 30; // 让 kill -9 大概率落进"处理到一半"的两个窗口
    ShmChannelServer server(group, ep, Ret(), handler);
    if (server.Start() != 0)
        return 32;
    for (;;)
        ::pause(); // 等着被杀；SpawnClient 里的 alarm 兜底
    return 0;
}

XprocChildren SpawnXproc(const Endpoint& kill_ep, const Endpoint& agent_ep)
{
    XprocChildren c;
    c.app_a = SpawnClient(
        [&]
        {
            auto r = ShmChannelClient::Connect(ClientOpts(kill_ep, "xp-app"));
            if (!r.has_value())
                return 11;
            SendRange(*r, 0, 100);
            ::kill(::getpid(), SIGKILL);
            return 12;
        });
    c.app_b = SpawnClient(
        [&]
        {
            auto r = ShmChannelClient::Connect(ClientOpts(kill_ep, "xp-app"));
            if (!r.has_value())
                return 21;
            if (!r->resumed() || r->app_epoch() != 2)
                return 22;
            for (std::uint64_t want = 0; want < 100; ++want)
            {
                shm::RecordView v;
                if (!RecvOne(*r, v))
                    return 23;
                std::uint64_t seq = 0;
                if (!Verify(v.payload, v.len, &seq) || seq != want)
                    return 24;
                r->rx().Pop();
            }
            EchoRoundTrips(*r, 50, 1000);
            return 0;
        });
    for (Child& a : c.agents)
        a = SpawnClient([&] { return AgentChild(agent_ep); });
    return c;
}

void RunAppKillScenario(const XprocChildren& c, const Endpoint& ep)
{
    EventLoopGroup group(EventLoopGroupOptions{1, 256});
    CHECK(group.Start() == 0);
    EchoHandler handler;
    ShmChannelServer server(group, ep, Ret(), handler);
    CHECK(server.Start() == 0);

    Go(c.app_a);
    int st = 0;
    CHECK(::waitpid(c.app_a.pid, &st, 0) == c.app_a.pid);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);
    // 进程被杀：内核关闭它的 UDS，Agent 读到 EOF，通道进入保留，遗言（100 条）被处理完。
    CHECK(WaitUntil([&] { return server.stats().detached_channels == 1; }));
    CHECK(WaitUntil([&] { return handler.echoed.load() == 100; }));
    ExpectTrace(handler, "xp-app", "OD");

    Go(c.app_b);
    ExpectExit0(c.app_b, "app-b");
    // 进程 B 正常退出后 UDS 又断了一次：通道第二次进入保留（O D R D）。
    CHECK(WaitUntil([&] { return handler.trace("xp-app") == "ODRD"; }));
    CHECK(server.stats().resumed == 1 && server.stats().opened == 1);
}

void RunAgentKillScenario(const XprocChildren& c, const Endpoint& ep)
{
    ShmClientOptions opts = ClientOpts(ep, "xp-agent");
    opts.rx_capacity = 4 * shm::kMinRingCapacity;
    // attempt 返回 errno（0 = 成功）。Agent 进程刚被放行，socket 还没建好时 ENOENT / ECONNREFUSED 是正常的。
    const auto connect_until_up = [&](auto&& attempt)
    {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        for (;;)
        {
            const int e = attempt();
            if (e == 0)
                return;
            if (std::chrono::steady_clock::now() >= deadline)
            {
                std::fprintf(stderr, "连不上新起的 Agent：errno=%d\n", e);
                std::abort();
            }
            std::this_thread::sleep_for(10ms);
        }
    };

    Go(c.agents[0]);
    std::optional<ShmChannelClient> app;
    connect_until_up(
        [&]
        {
            auto r = ShmChannelClient::Connect(opts);
            if (r.has_value())
                app.emplace(std::move(*r));
            return r.has_value() ? 0 : r.error();
        });

    constexpr std::uint64_t kTotal = 24000;
    constexpr std::uint64_t kWindow = 64;
    std::uint64_t sent = 0;
    std::uint64_t last_seen = 0;
    bool seen_any = false;
    std::uint64_t received = 0;
    int kills = 0;
    int kills_in_window = 0;
    std::uint64_t next_kill_at = 2500;

    const auto drain_one = [&](std::chrono::milliseconds wait) -> bool
    {
        shm::RecordView v;
        if (!RecvOne(*app, v, wait))
            return false;
        std::uint64_t seq = 0;
        CHECK(Verify(v.payload, v.len, &seq));
        CHECK(v.len == LenOf(seq));
        // 不重复、不乱序（间隔里丢的是被杀那一刻处理到一半的那一条）。
        CHECK(!seen_any || seq > last_seen);
        seen_any = true;
        last_seen = seq;
        app->rx().Pop();
        ++received;
        return true;
    };

    // 用序号做流控：在途 = 已发 - 已见到的最大序号 - 1，丢了几条也不会让窗口卡住。
    const auto deadline = std::chrono::steady_clock::now() + 50s;
    while (!(sent == kTotal && seen_any && last_seen == kTotal - 1))
    {
        CHECK(std::chrono::steady_clock::now() < deadline);
        bool progressed = false;
        while (sent < kTotal)
        {
            const std::uint64_t outstanding = sent - (seen_any ? last_seen + 1 : 0);
            if (outstanding >= kWindow || !SendMsg(*app, sent, LenOf(sent)))
                break;
            ++sent;
            progressed = true;
        }
        if (kills < kAgentKills && received >= next_kill_at)
        {
            // Agent 此刻正在满负荷处理。触发 kill 的是"收到一条回显"，而回显恰恰是提交的那一刻，所以不加延迟的话
            // 永远落在"提交之后、Pop 之前"；按轮次错开几十微秒，才会落进"标记之后、提交之前"（那时会丢这一条）。
            SpinFor(20 + 17 * kills);
            CHECK(::kill(c.agents[kills].pid, SIGKILL) == 0);
            int st = 0;
            CHECK(::waitpid(c.agents[kills].pid, &st, 0) == c.agents[kills].pid);
            CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);
            CHECK(WaitUntil([&] { return app->AgentGone(); }));
            {
                // 被杀那一刻 Agent 有没有留下"处理到一半"的标记：标记等于 read_idx 说明落在处理窗口里。
                // 回显已提交（rx 的 write 比 tx 的 read 多一条）：新 Agent 跳过它，既不丢也不重；
                // 回显没提交：新 Agent 跳过它，丢这一条（至多一次）。
                const auto& tc = app->shm().tx_control();
                if (tc.inflight_idx.load() == tc.read_idx.load())
                    ++kills_in_window;
            }
            ++kills;
            next_kill_at = received + 3000;
            Go(c.agents[kills]);
            const std::uint64_t before = app->agent_epoch();
            connect_until_up([&] { return app->Reattach(opts); });
            CHECK(app->agent_epoch() == before + 1 && app->app_epoch() == 1);
            continue;
        }
        if (drain_one(progressed ? 0ms : 100ms))
            progressed = true;
    }
    CHECK(kills == kAgentKills);
    CHECK(sent == kTotal);
    CHECK(sent - received <= static_cast<std::uint64_t>(kills)); // 至多一次：每次被杀最多丢一条
    CHECK(received >= kTotal - kAgentKills);
    std::printf("shm_retention_test: kill -9 Agent %d 次（%d 次落在处理窗口里），发 %llu 收 %llu（丢 %llu）\n",
                kills,
                kills_in_window,
                static_cast<unsigned long long>(sent),
                static_cast<unsigned long long>(received),
                static_cast<unsigned long long>(sent - received));
    // 还活着的那个 Agent（以及没启动过、还在等放行的）都收掉，不然 waitpid 要等到它们的 alarm。
    for (std::size_t i = static_cast<std::size_t>(kills); i < c.agents.size(); ++i)
        ::kill(c.agents[i].pid, SIGKILL);
}

int RunCrossProcess()
{
    TempDir dir;
    const Endpoint kill_ep = dir.Unix("app.sock");
    const Endpoint agent_ep = dir.Unix("agent.sock");
    XprocChildren c = SpawnXproc(kill_ep, agent_ep);

    RunAppKillScenario(c, kill_ep);
    RunAgentKillScenario(c, agent_ep);

    ::close(c.app_a.go_fd);
    ::close(c.app_b.go_fd);
    for (Child& a : c.agents)
    {
        int st = 0;
        ::waitpid(a.pid, &st, 0);
        ::close(a.go_fd);
    }
    return 0;
}

} // namespace

int main()
{
    // 跨进程的用例放在最前面：子进程要在进程里还没有任何线程的时候 fork 出来。
    CHECK(RunCrossProcess() == 0);

    TestAppRestartResumes();
    TestRetentionExpires();
    TestStaleRetentionTimerAfterResume();
    TestDrainWithTinyBudgetLosesNothing();
    TestDrainTimeoutBoundsStuckChannel();
    TestPrepareRestart();
    TestBye();
    TestRawControlMessages();
    TestAppKilledBeforeReadingAccept();
    TestResumeRules();
    TestDiscardRetained();
    TestDiscardUnderTightQuota();
    TestResumeIdentity();
    TestRapidRestartNeverSeesBusy();
    TestHelloBeforeOldEofStillResumes();
    TestServerStopDestroysRetained();
    TestHandlerClosesDetachedChannel();
    TestReattachAfterAgentRestart();
    TestReattachWhileAgentStillHasChannel();
    TestReattachHostileInput();
    TestReattachTotalQuota();
    TestClientReattachAgainstHostileAgent();
    std::puts("shm_retention_test: OK");
    return 0;
}
