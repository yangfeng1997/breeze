// 共享内存通道的握手、服务端与客户端（设计见 docs/共享内存IPC设计.md §5 §6.1）。
//
// 进程内的用例用 EventLoopGroup + ShmChannelServer + ShmChannelClient；
// 跨进程的用例（真正的 SCM_RIGHTS、kill -9）在 main 一开头、任何线程创建之前 fork 出子进程：
// 多线程进程里 fork 之后子进程只能做异步信号安全的事，所以子进程必须在没有别的线程时诞生。
//
// 无测试框架依赖：失败即 abort，并发用例带超时。

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "base/net/loop/group_access.h"
#include "base/net/shm/shm_channel_client.h"
#include "base/net/shm/shm_channel_server.h"
#include "base/net/shm/shm_protocol.h"
#include "base/net/stream/internal/fd_passing.h"
#include "base/net/testing/net_test_util.h"
#include "base/shm/shm_memfd.h"

#include "base/net/testing/shm_channel_test_util.h"

using namespace shmtest;

namespace
{

// ---- 用例 ----

void TestHandshakeAndEcho()
{
    LeakCheck leak;
    Env env;
    {
        ShmClientOptions o;
        o.endpoint = env.ep;
        o.busid = "app-1";
        o.timeout = 5s; // 容量全用默认值（16 MiB × 2）
        auto r = ShmChannelClient::Connect(o);
        CHECK(r.has_value());
        ShmChannelClient& c = *r;
        CHECK(c.valid() && c.liveness_fd() >= 0);
        CHECK(c.shm().layout().tx.capacity == shm::kDefaultRingCapacity);
        CHECK(c.shm().layout().rx.capacity == shm::kDefaultRingCapacity);
        CHECK(c.app_epoch() == 1 && c.agent_epoch() == 1);
        CHECK(c.shm().header().app_epoch.load() == 1 && c.shm().header().agent_epoch.load() == 1);
        CHECK(!c.AgentGone());

        CHECK(WaitUntil([&] { return env.handler.opened_count() == 1; }));
        const auto admits = env.handler.admits();
        CHECK(admits.size() == 1 && admits[0].busid == "app-1");
        CHECK(admits[0].peer.pid == ::getpid() && admits[0].peer.uid == ::getuid()); // SO_PEERCRED，不是自称的
        CHECK(admits[0].claimed_pid == ::getpid());
        const ShmServerStats st = env.server->stats();
        CHECK(WaitUntil([&] { return env.server->stats().open_channels == 1; }));
        CHECK(st.rejected == 0);

        EchoRoundTrips(c, 2000);
    }
    CHECK(WaitUntil([&] { return env.handler.closed_reason("app-1") == ShmCloseReason::PeerClosed; }));
    CHECK(WaitUntil([&] { return env.server->stats().open_channels == 0 && env.server->stats().closed == 1; }));
}

// 突发加停顿：消费者（服务端）反复睡下去再被 eventfd 叫醒，走完整的睡前协议；
// 共享内存里的计数能证明 eventfd 确实起了作用（不是一路自旋过来的）。
void TestWakeupAcrossSleeps()
{
    LeakCheck leak;
    Env env;
    ShmChannelClient c = MustConnect(env.ep, "app-wake");
    for (int round = 0; round < 40; ++round)
    {
        EchoRoundTrips(c, 20, static_cast<std::uint64_t>(round) * 1000);
        std::this_thread::sleep_for(3ms); // 让服务端的消费循环搬空并睡下去
    }
    const shm::RingControl& tx = c.shm().tx_control();
    CHECK(tx.producer_stats.wakeups_sent.Get() > 0);     // App 的 Commit 敲过 eventfd
    CHECK(tx.consumer_stats.wakeups_received.Get() > 0); // 服务端被叫醒过
    CHECK(tx.producer_stats.records_committed.Get() == 800);
    CHECK(tx.consumer_stats.records_popped.Get() == 800);
}

// 回调每次只处理一条（预算 1）：库要把"没处理完"的通道让出再调回来，一条都不能丢，也不能卡住。
void TestBudgetYieldsAndResumes()
{
    LeakCheck leak;
    Env env;
    env.handler.budget.store(1);
    env.handler.loop = &GroupAccess::LoopAt(env.group, 0);
    ShmChannelClient c = MustConnect(env.ep, "app-budget");
    constexpr std::uint64_t kN = 500;
    for (std::uint64_t i = 0; i < kN; ++i)
        CHECK(SendMsg(c, i, LenOf(i))); // 每条一次 Commit，积压 500 条
    for (std::uint64_t i = 0; i < kN; ++i)
    {
        shm::RecordView v;
        CHECK(RecvOne(c, v));
        std::uint64_t seq = 0;
        CHECK(Verify(v.payload, v.len, &seq) && seq == i);
        c.rx().Pop();
    }
    CHECK(env.handler.readable_calls.load() >= kN); // 预算 1：至少每条一次调用
    // 预算用完时库必须让出、下一轮再来，不能在同一轮事件循环里一直调下去独占 loop。
    // 正确实现下最多 5 次（一次事件分发加 EventLoop 每轮最多 4 批任务）；不让出的实现实测是 40。
    CHECK(env.handler.max_calls_in_iteration.load() <= 8);
}

void TestManyClients()
{
    LeakCheck leak;
    Env env;
    constexpr int kClients = 6;
    std::atomic<int> ok{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < kClients; ++i)
    {
        ts.emplace_back(
            [&, i]
            {
                ShmChannelClient c = MustConnect(env.ep, "app-m" + std::to_string(i));
                EchoRoundTrips(c, 300, static_cast<std::uint64_t>(i) * 100000);
                ok.fetch_add(1);
            });
    }
    for (auto& t : ts)
        t.join();
    CHECK(ok.load() == kClients);
    CHECK(WaitUntil([&] { return env.server->stats().opened == kClients && env.server->stats().open_channels == 0; }));
}

void TestDuplicateBusIdAndReuse()
{
    LeakCheck leak;
    Env env;
    std::optional<ShmChannelClient> first = MustConnect(env.ep, "app-dup");
    CHECK(ConnectErr(env.ep, "app-dup") == EBUSY);
    EchoRoundTrips(*first, 10); // 被拒的那一次不影响已有的通道

    first.reset();
    CHECK(WaitUntil([&] { return env.handler.closed_reason("app-dup").has_value(); }));
    // 服务端处理完 EOF 之后名字就空出来了：同名重连成功。
    ShmChannelClient second = MustConnect(env.ep, "app-dup");
    CHECK(second.agent_epoch() == 1 && second.app_epoch() == 1); // 第一版没有保留：是全新的通道
    EchoRoundTrips(second, 10);
}

void TestRejections()
{
    LeakCheck leak;
    { // 容量不合法：不是 2 的幂 / 小于下限。
        Env env;
        CHECK(ConnectErr(env.ep, "r1", 3 * shm::kMinRingCapacity) == EINVAL);
        CHECK(ConnectErr(env.ep, "r1", shm::kMinRingCapacity / 2) == EINVAL);
        CHECK(env.handler.opened_count() == 0);
        MustConnect(env.ep, "r1"); // 被拒之后服务端照常工作，名字也没被占着
    }
    { // 容量超过服务端的单环上限。
        ShmServerOptions o = NoRetention();
        o.max_ring_capacity = shm::kMinRingCapacity;
        Env env(o);
        CHECK(ConnectErr(env.ep, "r2", 2 * shm::kMinRingCapacity) == EINVAL);
        MustConnect(env.ep, "r2");
    }
    { // 通道数配额。
        ShmServerOptions o = NoRetention();
        o.max_channels = 2;
        Env env(o);
        ShmChannelClient a = MustConnect(env.ep, "q-a");
        ShmChannelClient b = MustConnect(env.ep, "q-b");
        CHECK(WaitUntil([&] { return env.server->stats().open_channels == 2; }));
        CHECK(ConnectErr(env.ep, "q-c") == ENOSPC);
        EchoRoundTrips(a, 5);
    }
    { // 共享内存总量配额：每条通道约 2 MiB + 12 KiB，上限 3 MiB 只放得下一条。
        ShmServerOptions o = NoRetention();
        o.max_total_bytes = 3ULL << 20;
        Env env(o);
        ShmChannelClient a = MustConnect(env.ep, "m-a");
        CHECK(ConnectErr(env.ep, "m-b") == ENOSPC);
        a = ShmChannelClient{}; // 放掉第一条，配额还回来
        CHECK(WaitUntil([&] { return env.server->stats().open_channels == 0; }));
        MustConnect(env.ep, "m-b");
    }
    { // 准入：身份 / 策略。OnAdmit 返回的 errno 原样回给对端；拒绝发生在创建共享内存之前。
        Env env;
        env.handler.admit_deny.store(EACCES);
        CHECK(ConnectErr(env.ep, "deny-me") == EACCES);
        CHECK(env.handler.opened_count() == 0);
        CHECK(WaitUntil([&] { return env.server->stats().rejected == 1; }));
        env.handler.admit_deny.store(0);
        MustConnect(env.ep, "deny-me");
    }
    { // 客户端自己就拒绝不合法的通道名与端点，不会去连。
        Env env;
        CHECK(ConnectErr(env.ep, "bad name") == EINVAL);
        CHECK(ConnectErr(env.ep, "") == EINVAL);
        CHECK(ConnectErr(env.ep, std::string(64, 'x')) == EINVAL);
        CHECK(env.server->stats().rejected == 0);
        ShmClientOptions o = ClientOpts(*Endpoint::Tcp("127.0.0.1", 1), "tcp");
        auto r = ShmChannelClient::Connect(o);
        CHECK(!r.has_value() && r.error() == EPROTONOSUPPORT);
    }
}

void TestRawProtocolAbuse()
{
    LeakCheck leak;
    Env env;

    // 各种不合格的 Hello：服务端都回一条带 errno 的 Reject（不带 fd），然后关连接。
    auto with = [&](auto mutate)
    {
        wire::Hello h = MakeHello();
        mutate(h);
        return RawHandshake(env.ep, h);
    };
    CHECK(with([](wire::Hello& h) { h.hdr.version = wire::kVersion + 1; }) == EPROTO);
    CHECK(with([](wire::Hello& h) { h.hdr.magic = 0; }) == EPROTO);
    CHECK(with([](wire::Hello& h) { h.hdr.type = static_cast<std::uint16_t>(wire::MsgType::kReply); }) == EPROTO);
    // Reattach 不带容量（MakeHello 填了容量）；没带 fd 的 Reattach 也过不了。
    CHECK(with([](wire::Hello& h) { h.attach_type = static_cast<std::uint32_t>(wire::AttachType::kReattach); }) ==
          EINVAL);
    CHECK(with([](wire::Hello& h) { h.attach_type = 7; }) == EINVAL);
    CHECK(with([](wire::Hello& h) { h.flags = 2; }) == EINVAL); // 1 是 discard_retained，合法
    CHECK(with([](wire::Hello& h) { std::memset(h.busid, 'a', sizeof(h.busid)); }) == EINVAL); // 没有结尾
    CHECK(with([](wire::Hello& h) { h.busid[0] = '/'; }) == EINVAL);
    CHECK(with([](wire::Hello& h) { h.tx_capacity = 12345; }) == EINVAL); // 容量在创建通道时校验
    CHECK(with([](wire::Hello& h) { h.tx_max_payload = 0xFFFFFFFF; }) == EINVAL);
    CHECK(env.handler.opened_count() == 0);
    CHECK(WaitUntil([&] { return env.server->stats().rejected == 10; }));

    // 全零垃圾（长度够一条 Hello）。
    {
        UniqueFd fd = RawConnect(env.ep);
        std::array<std::byte, sizeof(wire::Hello)> zeros{};
        RawSend(fd.get(), zeros.data(), zeros.size());
        auto r = RawRecvReply(fd.get());
        CHECK(r.has_value() && r->reply.result == EPROTO);
        CHECK(RawSeesEof(fd.get()));
    }

    // Attach 的 Hello 不该带 fd：带了就 Reject(EPROTO)，fd 随连接关闭（LeakCheck 在作用域结束时核对服务端的 fd 表）。
    {
        std::vector<UniqueFd> junk;
        std::array<int, 3> raw{};
        for (int& r : raw)
        {
            int p[2];
            CHECK(::pipe2(p, O_CLOEXEC) == 0);
            UniqueFd rd(p[0]);
            junk.emplace_back(p[1]);
            r = p[1];
        }
        UniqueFd fd = RawConnect(env.ep);
        const wire::Hello h = MakeHello("with-junk-fds");
        CHECK(sock::SendWithFds(fd.get(), std::as_bytes(std::span(&h, 1)), raw) == 0);
        auto r = RawRecvReply(fd.get());
        CHECK(r.has_value() && r->reply.result == EPROTO && r->nfds == 0);
        CHECK(RawSeesEof(fd.get()));
        CHECK(env.handler.opened_count() == 0);
    }
    // 带的 fd 比服务端留的槽位（4 个）还多：整条当协议错误，不回复，直接断开，已收到的 fd 关闭。
    {
        std::vector<UniqueFd> junk;
        std::array<int, sock::kMaxPassedFds> raw{};
        for (int& r : raw)
        {
            int p[2];
            CHECK(::pipe2(p, O_CLOEXEC) == 0);
            UniqueFd rd(p[0]);
            junk.emplace_back(p[1]);
            r = p[1];
        }
        UniqueFd fd = RawConnect(env.ep);
        const wire::Hello h = MakeHello("too-many-fds");
        CHECK(sock::SendWithFds(fd.get(), std::as_bytes(std::span(&h, 1)), raw) == 0);
        CHECK(RawSeesEof(fd.get()));
        CHECK(WaitUntil([&] { return env.server->stats().protocol_errors == 1; }));
        CHECK(env.handler.opened_count() == 0);
    }

    // Hello 分两段到、每段各带 3 个 fd：每次 recvmsg 都没超过槽位，但累计 6 个超过了 4 个槽位——要当协议错误
    // （没有这一道检查会写出槽位数组，asan 下立刻报）。
    {
        std::vector<UniqueFd> junk;
        std::array<int, 3> raw{};
        for (int& r : raw)
        {
            int p[2];
            CHECK(::pipe2(p, O_CLOEXEC) == 0);
            UniqueFd rd(p[0]);
            junk.emplace_back(p[1]);
            r = p[1];
        }
        UniqueFd fd = RawConnect(env.ep);
        const wire::Hello h = MakeHello("two-segments-fds");
        CHECK(sock::SendWithFds(fd.get(), std::as_bytes(std::span(&h, 1)).first(10), raw) == 0);
        std::this_thread::sleep_for(30ms); // 让服务端先收下第一段
        CHECK(sock::SendWithFds(fd.get(), std::as_bytes(std::span(&h, 1)).subspan(10), raw) == 0);
        CHECK(RawSeesEof(fd.get()));
        CHECK(WaitUntil([&] { return env.server->stats().protocol_errors == 2; }));
    }

    // 握手完成之后对端往 UDS 里写不是控制消息的字节是协议错误：关闭，OnClosed(ProtocolError)。
    {
        UniqueFd fd = RawConnect(env.ep);
        const wire::Hello h = MakeHello("talker");
        RawSend(fd.get(), &h, sizeof(h));
        auto r = RawRecvReply(fd.get());
        CHECK(r.has_value() && r->reply.result == 0);
        RawSend(fd.get(), "x", 1);
        CHECK(WaitUntil([&] { return env.handler.closed_reason("talker") == ShmCloseReason::ProtocolError; }));
        CHECK(env.server->stats().protocol_errors == 3); // 上面"fd 太多"与"分两段累计太多"各一次
        CHECK(RawSeesEof(fd.get()));
    }

    // 只发了半条 Hello 就走了：悄悄丢掉，没有 OnOpened、不占配额、不泄漏。
    {
        UniqueFd fd = RawConnect(env.ep);
        const wire::Hello h = MakeHello("half");
        RawSend(fd.get(), &h, sizeof(h) / 2);
    }
    // Hello 分两次发过来（拆成两段）：照常握手。
    {
        UniqueFd fd = RawConnect(env.ep);
        const wire::Hello h = MakeHello("split");
        RawSend(fd.get(), &h, 10);
        std::this_thread::sleep_for(20ms);
        RawSend(fd.get(), reinterpret_cast<const char*>(&h) + 10, sizeof(h) - 10);
        auto r = RawRecvReply(fd.get());
        CHECK(r.has_value() && r->reply.result == 0 && r->nfds == 3);
    }
    CHECK(WaitUntil([&] { return env.server->stats().open_channels == 0; }));
}

void TestHandshakeTimeoutAndPendingLimit()
{
    LeakCheck leak;
    ShmServerOptions o = NoRetention();
    o.handshake_timeout = 200ms;
    o.max_pending_handshakes = 2;
    Env env(o);

    // 连上之后一个字节都不发：超时被断开。
    {
        UniqueFd fd = RawConnect(env.ep);
        CHECK(RawSeesEof(fd.get(), 5s));
        CHECK(env.server->stats().handshake_timeouts == 1);
    }

    // 待握手的连接数上限：占满 2 个，第 3 个被直接断开；占着的断开之后又能连。
    {
        UniqueFd a = RawConnect(env.ep);
        UniqueFd b = RawConnect(env.ep);
        CHECK(WaitUntil([&] { return env.server->stats().handshake_timeouts == 1; })); // 前面那个已计入
        UniqueFd c = RawConnect(env.ep);
        // 三个里最多两个能占住名额：第三个要么被立即断开（溢出），要么在 a / b 超时之后也被断开；
        // 溢出计数 >= 1 说明上限确实起了作用。
        CHECK(WaitUntil([&] { return env.server->stats().pending_overflow >= 1; }));
        CHECK(RawSeesEof(a.get()) && RawSeesEof(b.get()));
    }
    ShmChannelClient ok = MustConnect(env.ep, "after-overflow"); // 名额释放之后恢复正常
    EchoRoundTrips(ok, 5);

    // 握手完成的通道不再占"待握手"名额：上限是 2，但可以同时开着更多通道。
    std::vector<ShmChannelClient> many;
    for (int i = 0; i < 4; ++i)
        many.push_back(MustConnect(env.ep, "keep-" + std::to_string(i)));
    CHECK(WaitUntil([&] { return env.server->stats().open_channels == 5; }));
}

// 握手到一半对端就走了：服务端读到 EOF 要马上关掉这条连接，不用干等握手超时（默认 5 秒）。
void TestHalfHelloClosesPromptly()
{
    LeakCheck leak;
    Env env;
    const int baseline = CountEntries("/proc/self/fd");
    {
        UniqueFd fd = RawConnect(env.ep);
        const wire::Hello h = MakeHello("half");
        RawSend(fd.get(), &h, sizeof(h) / 2);
        // 先等服务端真的接受了这条连接（fd 表里多出它那一端）：否则"回到基线"可能只是因为它还没被接受，会空等通过。
        CHECK(WaitUntil([&] { return CountEntries("/proc/self/fd") == baseline + 2; }));
    }
    CHECK(WaitUntil([&] { return CountEntries("/proc/self/fd") == baseline; }, 2s));
    CHECK(env.server->stats().handshake_timeouts == 0); // 是 EOF 触发的关闭，不是超时
    CHECK(env.server->stats().open_channels == 0 && env.handler.opened_count() == 0);
}

void TestServerStop()
{
    LeakCheck leak;
    Env env;
    ShmChannelClient c = MustConnect(env.ep, "app-stop");
    EchoRoundTrips(c, 10);
    CHECK(!c.AgentGone());

    env.server->Stop();
    CHECK(WaitUntil([&] { return c.AgentGone(); }));
    CHECK(WaitUntil([&] { return env.handler.closed_reason("app-stop") == ShmCloseReason::ServerStopped; }));
    { // 探活 fd 变为可读，可以直接挂进 epoll。
        pollfd pfd{c.liveness_fd(), POLLIN, 0};
        CHECK(::poll(&pfd, 1, 1000) == 1 && (pfd.revents & (POLLIN | POLLHUP | POLLRDHUP)) != 0);
    }
    // 监听已停（socket 文件已被删掉）：新连接进不来。
    const int e = ConnectErr(env.ep, "late");
    CHECK(e == ENOENT || e == ECONNREFUSED);
    // Agent 没了之后 App 手里的共享内存仍然可读写（Tx 环照常能写到满）——这是 Reattach 的前提。
    CHECK(SendMsg(c, 1, 64));
}

// 客户端写坏了环（记录头保留位非零）：服务端的消费者发现 Corrupt，回调里 Close()，通道被销毁，客户端看到断开。
void TestCorruptRingClosesChannel()
{
    LeakCheck leak;
    Env env;
    ShmChannelClient c = MustConnect(env.ep, "app-corrupt");
    auto r = c.tx().TryReserve(40);
    CHECK(r.status == shm::ReserveStatus::kOk);
    Fill(r.payload, 1);
    reinterpret_cast<shm::RecordHeader*>(r.payload.data() - shm::kRecordHeaderSize)->reserved = 7;
    c.tx().Commit();
    CHECK(WaitUntil([&] { return env.handler.closed_reason("app-corrupt") == ShmCloseReason::LocalClose; }));
    CHECK(env.handler.corrupt_seen.load() == 1);
    CHECK(WaitUntil([&] { return c.AgentGone(); }));
}

void TestStartAndConnectErrors()
{
    LeakCheck leak;
    TempDir dir;
    EventLoopGroup group(EventLoopGroupOptions{1, 64});
    CHECK(group.Start() == 0);
    EchoHandler handler;

    // fd 只能经 UDS 传：TCP 端点不收。
    ShmChannelServer tcp_server(group, *Endpoint::Tcp("127.0.0.1", 0), {}, handler);
    CHECK(tcp_server.Start() == EPROTONOSUPPORT);

    const Endpoint ep = dir.Unix("a.sock");
    ShmChannelServer first(group, ep, {}, handler);
    CHECK(first.Start() == 0);
    ShmChannelServer second(group, ep, {}, handler);
    CHECK(second.Start() == EADDRINUSE); // 同一个路径已经有人在监听

    // 没人监听的路径。
    TempDir other;
    const int e = ConnectErr(other.Unix("nobody.sock"), "x");
    CHECK(e == ENOENT || e == ECONNREFUSED);
}

// ---- 假 Agent：测客户端面对不规矩的 Agent ----

// 监听一个 UDS，只服务一个连接：收齐一条 Hello，交给脚本去回复。Agent 也是不可信的一方：
// 客户端对它给的 Reply、fd 数量、fd 的种类、布局一样要逐项校验。
void TestClientAgainstHostileAgent()
{
    LeakCheck leak;
    auto good = [](int conn, const wire::Hello&)
    {
        shm::ShmChannel ch = NewShm();
        const std::array<int, 3> fds{ch.memfd(), ch.tx_eventfd(), ch.rx_eventfd()};
        SendReply(conn, GoodReply(ch), fds);
    };

    // 基线：规矩的假 Agent 能握手成功，并且客户端发出的 Hello 内容正确。
    wire::Hello seen{};
    CHECK(ConnectToFake(good, 5s, {}, &seen) == 0);
    CHECK(wire::CheckHello(seen) == 0);
    CHECK(wire::GetBusId(seen).value() == "fake-app");
    CHECK(seen.pid == static_cast<std::int32_t>(::getpid()));
    CHECK(seen.attach_type == static_cast<std::uint32_t>(wire::AttachType::kAttach));
    CHECK(seen.flags == 0 && seen.transport == static_cast<std::uint32_t>(wire::Transport::kShmRequired));
    CHECK(seen.tx_capacity == shm::kMinRingCapacity && seen.rx_capacity == shm::kMinRingCapacity);

    // 回复的头不对：魔数、版本、类型。
    auto bad_header = [](auto mutate)
    {
        return [mutate](int conn, const wire::Hello&)
        {
            shm::ShmChannel ch = NewShm();
            wire::Reply r = GoodReply(ch);
            mutate(r);
            const std::array<int, 3> fds{ch.memfd(), ch.tx_eventfd(), ch.rx_eventfd()};
            SendReply(conn, r, fds);
        };
    };
    CHECK(ConnectToFake(bad_header([](wire::Reply& r) { r.hdr.magic = 0; })) == EPROTO);
    CHECK(ConnectToFake(bad_header([](wire::Reply& r) { r.hdr.version = wire::kVersion + 1; })) == EPROTO);
    CHECK(ConnectToFake(bad_header([](wire::Reply& r)
                                   { r.hdr.type = static_cast<std::uint16_t>(wire::MsgType::kHello); })) == EPROTO);

    // 回复里的布局与共享内存头里的对不上：以头为准，对不上就拒绝（Agent 或共享内存不对劲）。
    CHECK(ConnectToFake(bad_header([](wire::Reply& r) { r.total_size += 4096; })) == EPROTO);
    CHECK(ConnectToFake(bad_header([](wire::Reply& r) { r.tx.capacity *= 2; })) == EPROTO);
    CHECK(ConnectToFake(bad_header([](wire::Reply& r) { r.rx.max_payload += 1; })) == EPROTO);
    CHECK(ConnectToFake(bad_header([](wire::Reply& r) { r.tx.max_payload += 1; })) == EPROTO);
    CHECK(ConnectToFake(bad_header([](wire::Reply& r) { r.rx.capacity *= 2; })) == EPROTO);
    CHECK(ConnectToFake(bad_header([](wire::Reply& r) { r.tx.reserved = 1; })) == EPROTO);
    CHECK(ConnectToFake(bad_header([](wire::Reply& r) { r.resumed = 2; })) == EPROTO); // 只有 0 / 1
    CHECK(ConnectToFake(bad_header([](wire::Reply& r) { r.reserved = 1; })) == EPROTO);
    // 我们要的是共享内存：Agent 回了别的传输方式（或 0）就是协议错误。
    CHECK(ConnectToFake(bad_header([](wire::Reply& r)
                                   { r.transport = static_cast<std::uint32_t>(wire::ChosenTransport::kUds); })) ==
          EPROTO);
    CHECK(ConnectToFake(bad_header([](wire::Reply& r) { r.transport = 0; })) == EPROTO);

    // Reject：原因原样返回；Agent 如果在 Reject 上还附带了 fd，客户端也要把它们关掉（LeakCheck 核对）。
    CHECK(ConnectToFake(
              [](int conn, const wire::Hello&)
              {
                  shm::ShmChannel ch = NewShm();
                  wire::Reply r = GoodReply(ch);
                  r.result = ENOSPC;
                  const std::array<int, 3> fds{ch.memfd(), ch.tx_eventfd(), ch.rx_eventfd()};
                  SendReply(conn, r, fds);
              }) == ENOSPC);

    // fd 的数量不对：少、多、没有。
    auto with_fds = [](std::size_t count)
    {
        return [count](int conn, const wire::Hello&)
        {
            shm::ShmChannel ch = NewShm();
            std::vector<UniqueFd> extra;
            std::vector<int> fds{ch.memfd(), ch.tx_eventfd(), ch.rx_eventfd()};
            while (fds.size() < count)
            {
                extra.emplace_back(::dup(ch.memfd()));
                fds.push_back(extra.back().get());
            }
            fds.resize(count);
            SendReply(conn, GoodReply(ch), fds);
        };
    };
    CHECK(ConnectToFake(with_fds(2)) == EPROTO);
    CHECK(ConnectToFake(with_fds(4)) == EPROTO);
    CHECK(ConnectToFake(with_fds(8)) == EPROTO); // 超出客户端给的槽位：协议错误，已收到的也关闭
    CHECK(ConnectToFake(with_fds(0)) == EPROTO);
    // fd 分两段到、总数（3 + 3）超出客户端的槽位（4）：协议错误，已收到的全部关闭。
    // 这一条同时守住"不越界写槽位数组"：靠最后的"不是 3 个"也会返回 EPROTO，所以只有 asan 能抓到越界。
    CHECK(ConnectToFake(
              [](int conn, const wire::Hello&)
              {
                  shm::ShmChannel ch = NewShm();
                  const wire::Reply r = GoodReply(ch);
                  const std::array<int, 3> fds{ch.memfd(), ch.tx_eventfd(), ch.rx_eventfd()};
                  const auto bytes = std::as_bytes(std::span(&r, 1));
                  CHECK(sock::SendWithFds(conn, bytes.first(20), fds) == 0);
                  std::this_thread::sleep_for(50ms);
                  CHECK(sock::SendWithFds(conn, bytes.subspan(20), fds) == 0);
              }) == EPROTO);

    // fd 的种类不对：换了顺序（eventfd 当 memfd）、管道当 memfd、没封印的 memfd。
    CHECK(ConnectToFake(
              [](int conn, const wire::Hello&)
              {
                  shm::ShmChannel ch = NewShm();
                  const std::array<int, 3> fds{ch.tx_eventfd(), ch.memfd(), ch.rx_eventfd()};
                  SendReply(conn, GoodReply(ch), fds);
              }) == EINVAL);
    CHECK(ConnectToFake(
              [](int conn, const wire::Hello&)
              {
                  shm::ShmChannel ch = NewShm();
                  int p[2];
                  CHECK(::pipe(p) == 0);
                  UniqueFd r(p[0]), w(p[1]);
                  const std::array<int, 3> fds{r.get(), ch.tx_eventfd(), ch.rx_eventfd()};
                  SendReply(conn, GoodReply(ch), fds);
              }) == EINVAL);
    CHECK(ConnectToFake(
              [](int conn, const wire::Hello&)
              {
                  shm::ShmChannel ch = NewShm();
                  auto unsealed = shm::CreateMemFd("unsealed", ch.layout().total_size);
                  CHECK(unsealed.has_value());
                  const std::array<int, 3> fds{unsealed->get(), ch.tx_eventfd(), ch.rx_eventfd()};
                  SendReply(conn, GoodReply(ch), fds);
              }) == EINVAL);

    // 容量超过客户端自己设的上限（对 Agent 的信任程度由使用方定）。
    shm::AttachLimits tight;
    tight.max_ring_capacity = shm::kMinRingCapacity;
    CHECK(ConnectToFake(
              [](int conn, const wire::Hello&)
              {
                  shm::ShmChannel ch = NewShm(2 * shm::kMinRingCapacity);
                  const std::array<int, 3> fds{ch.memfd(), ch.tx_eventfd(), ch.rx_eventfd()};
                  SendReply(conn, GoodReply(ch), fds);
              },
              5s,
              tight) == EINVAL);

    // 连接建立之后 Agent 没回复就关了 / 只回了一半就关了 / 一直不回。
    CHECK(ConnectToFake([](int, const wire::Hello&) {}) == ECONNRESET);
    CHECK(ConnectToFake(
              [](int conn, const wire::Hello&)
              {
                  const wire::Reply r{};
                  CHECK(::send(conn, &r, 10, MSG_NOSIGNAL) == 10);
              }) == ECONNRESET);
    CHECK(ConnectToFake(
              [](int conn, const wire::Hello&)
              {
                  pollfd p{conn, POLLIN, 0};
                  (void)::poll(&p, 1, 1500);
              }, // 一直不回，直到客户端放弃
              300ms) == ETIMEDOUT);

    // Reply 分两段到：fd 随第一段，客户端要能拼起来。
    CHECK(ConnectToFake(
              [](int conn, const wire::Hello&)
              {
                  shm::ShmChannel ch = NewShm();
                  const wire::Reply r = GoodReply(ch);
                  const std::array<int, 3> fds{ch.memfd(), ch.tx_eventfd(), ch.rx_eventfd()};
                  CHECK(sock::SendWithFds(conn, std::as_bytes(std::span(&r, 1)).first(20), fds) == 0);
                  std::this_thread::sleep_for(50ms);
                  CHECK(sock::SendWithFds(conn, std::as_bytes(std::span(&r, 1)).subspan(20), {}) == 0);
              }) == 0);
}

// Agent 在握手之后往 UDS 里写了东西：协议里握手之后只有 EOF，当作断开。
void TestAgentDataAfterHandshakeMeansGone()
{
    LeakCheck leak;
    TempDir dir;
    const Endpoint ep = dir.Unix("fake.sock");
    FakeAgent fake(ep,
                   [](int conn, const wire::Hello&)
                   {
                       shm::ShmChannel ch = NewShm();
                       const std::array<int, 3> fds{ch.memfd(), ch.tx_eventfd(), ch.rx_eventfd()};
                       SendReply(conn, GoodReply(ch), fds);
                       CHECK(::send(conn, "x", 1, MSG_NOSIGNAL) == 1);
                       pollfd p{conn, POLLIN, 0};
                       (void)::poll(&p, 1, 5000); // 等客户端关掉
                   });
    auto r = ShmChannelClient::Connect(ClientOpts(ep, "fake-app"));
    CHECK(r.has_value());
    // 要立刻成立：假 Agent 此刻还握着连接（最多再等 5 秒才会自己关），窗口放得比这个宽，
    // 把数据当"还活着"的实现会等到对端自己关了才通过，测不出差别。
    CHECK(WaitUntil([&] { return r->AgentGone(); }, 2s));
}

// 对端的 backlog 满了：非阻塞 connect 返回 EAGAIN，客户端要在时限内稍等重试，
// 到点报超时，而不是直接把 EAGAIN 抛给调用方。
void TestConnectRetriesWhenBacklogFull()
{
    LeakCheck leak;
    TempDir dir;
    const Endpoint ep = dir.Unix("full.sock");
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::strncpy(a.sun_path, ep.address().c_str(), sizeof(a.sun_path) - 1);
    UniqueFd listener(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    CHECK(listener.valid());
    CHECK(::bind(listener.get(), reinterpret_cast<const sockaddr*>(&a), sizeof(a)) == 0);
    CHECK(::listen(listener.get(), 1) == 0); // 不 accept：队列很快就满
    std::vector<UniqueFd> fillers;
    for (int i = 0; i < 4; ++i)
    {
        UniqueFd c(::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
        (void)::connect(c.get(), reinterpret_cast<const sockaddr*>(&a), sizeof(a)); // 后几个会 EAGAIN
        fillers.push_back(std::move(c));
    }
    ShmClientOptions o = ClientOpts(ep, "late");
    o.timeout = 300ms;
    const auto t0 = std::chrono::steady_clock::now();
    auto r = ShmChannelClient::Connect(o);
    CHECK(!r.has_value() && r.error() == ETIMEDOUT);
    CHECK(std::chrono::steady_clock::now() - t0 >= 250ms); // 确实等满了时限
}

// ---- 跨进程：真正的 SCM_RIGHTS 与 kill -9 ----

// 回显、被 kill -9、被 kill 之后同名顶替，各一个 App 进程；服务端在本进程。
// 子进程在任何线程创建之前 fork（见文件头）。
int RunCrossProcess()
{
    TempDir dir;
    const Endpoint ep = dir.Unix("agent.sock");

    const Child echo = SpawnClient(
        [&]
        {
            auto r = ShmChannelClient::Connect(ClientOpts(ep, "xproc-echo"));
            if (!r.has_value())
                return 11;
            EchoRoundTrips(*r, 3000);
            return 0;
        });
    const Child kill_self = SpawnClient(
        [&]
        {
            auto r = ShmChannelClient::Connect(ClientOpts(ep, "xproc-kill"));
            if (!r.has_value())
                return 12;
            for (std::uint64_t i = 0; i < 100; ++i)
            {
                if (!SendMsg(*r, i, LenOf(i)))
                    return 13;
            }
            ::kill(::getpid(), SIGKILL); // 没有任何清理：模拟 App 崩溃
            return 14;
        });
    const Child replace = SpawnClient(
        [&]
        {
            auto r = ShmChannelClient::Connect(ClientOpts(ep, "xproc-kill"));
            if (!r.has_value())
                return 15; // 旧进程已被服务端发现退出，名字应当已经空出来
            EchoRoundTrips(*r, 100);
            return 0;
        });

    EventLoopGroup group(EventLoopGroupOptions{1, 256});
    CHECK(group.Start() == 0);
    EchoHandler handler;
    ShmChannelServer server(group, ep, NoRetention(), handler);
    CHECK(server.Start() == 0);

    Go(echo);
    ExpectExit0(echo, "echo");
    CHECK(WaitUntil([&] { return handler.closed_reason("xproc-echo") == ShmCloseReason::PeerClosed; }));
    CHECK(handler.echoed.load() >= 3000);

    Go(kill_self);
    int st = 0;
    CHECK(::waitpid(kill_self.pid, &st, 0) == kill_self.pid);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);
    // 进程被杀：内核关闭它的 UDS，服务端读到 EOF，销毁通道。
    CHECK(WaitUntil([&] { return handler.closed_reason("xproc-kill") == ShmCloseReason::PeerClosed; }));

    Go(replace);
    ExpectExit0(replace, "replace");
    CHECK(WaitUntil([&] { return server.stats().opened == 3; }));
    ::close(echo.go_fd);
    ::close(kill_self.go_fd);
    ::close(replace.go_fd);
    return 0;
}

} // namespace

int main()
{
    ::alarm(180);
    // 必须最先：此时还没有任何线程。
    RunCrossProcess();

    TestHandshakeAndEcho();
    TestWakeupAcrossSleeps();
    TestBudgetYieldsAndResumes();
    TestManyClients();
    TestDuplicateBusIdAndReuse();
    TestRejections();
    TestRawProtocolAbuse();
    TestHandshakeTimeoutAndPendingLimit();
    TestServerStop();
    TestCorruptRingClosesChannel();
    TestStartAndConnectErrors();
    TestClientAgainstHostileAgent();
    TestAgentDataAfterHandshakeMeansGone();
    TestConnectRetriesWhenBacklogFull();
    TestHalfHelloClosesPromptly();
    std::puts("shm_channel_test: OK");
    return 0;
}
