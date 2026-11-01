// 握手时协商传输方式（设计 §6.1）：
//   UdsOnly       只用 UDS：握手之后这条 UDS 交给 StreamConnection 当数据通道
//   ShmPreferred  要共享内存；Agent 不提供 / 给不了（内存配额满）时退化成仅 UDS
//   ShmRequired   要共享内存；给不了就拒绝
// 以及仅 UDS 的数据通道本身：收发、身份、通道名与配额在两种传输之间统一、停机。
//
// 无测试框架依赖：失败即 abort，并发用例带超时。

#include <sys/syscall.h>

#include <initializer_list>

#include "base/net/testing/shm_channel_test_util.h"

using namespace shmtest;

namespace
{

// RawConn / Handshake / SendFrame / RecvFrame 在 shm_channel_test_util.h 里（路由器的测试也用）。

// ---- 协商 ----

void TestNegotiationMatrix()
{
    LeakCheck leak;
    { // 提供共享内存，也提供仅 UDS 的数据通道。
        Env env(NoRetention(), true);
        RawConn u = Handshake(env.ep, "n-uds", Transport::kUdsOnly);
        CHECK(u.result() == 0 && u.uds() && u.reply.nfds == 0);
        CHECK(u.reply.reply.resumed == 0 && u.reply.reply.total_size == 0); // 没有共享内存：布局全零
        CHECK(u.reply.reply.tx.capacity == 0 && u.reply.reply.rx.capacity == 0);
        RawConn r = Handshake(env.ep, "n-shm-req", Transport::kShmRequired);
        CHECK(r.result() == 0 && r.shm() && r.reply.nfds == 3);
        RawConn p = Handshake(env.ep, "n-shm-pref", Transport::kShmPreferred);
        CHECK(p.result() == 0 && p.shm() && p.reply.nfds == 3); // 能给就给共享内存，不白白退化
        CHECK(WaitUntil([&] { return env.server->stats().open_channels == 3; }));
        const ShmServerStats st = env.server->stats();
        CHECK(st.uds_channels == 1 && st.uds_total == 1 && st.downgraded == 0 && st.opened == 3);
        CHECK(env.handler.opened_count() == 2); // OnOpened 只给共享内存的通道
        CHECK(env.uds_events.connected_count() == 1);
    }
    { // Agent 不提供共享内存：仅 UDS 照常，优先共享内存的退化，必须共享内存的拒绝。
        ShmServerOptions o = NoRetention();
        o.allow_shm = false;
        Env env(o, true);
        RawConn u = Handshake(env.ep, "a-uds", Transport::kUdsOnly);
        CHECK(u.result() == 0 && u.uds());
        RawConn p = Handshake(env.ep, "a-pref", Transport::kShmPreferred);
        CHECK(p.result() == 0 && p.uds() && p.reply.nfds == 0);
        CHECK(Handshake(env.ep, "a-req", Transport::kShmRequired).result() == ENOTSUP);
        CHECK(WaitUntil([&] { return env.server->stats().downgraded == 1; }));
        CHECK(env.server->stats().uds_total == 2 && env.handler.opened_count() == 0);
    }
    { // 没有提供仅 UDS 的数据通道：要仅 UDS 的拒绝；优先共享内存的在共享内存给不了时也只能拒绝。
        Env env(NoRetention(), false);
        CHECK(Handshake(env.ep, "z-uds", Transport::kUdsOnly).result() == ENOTSUP);
        ShmServerOptions o = NoRetention();
        o.allow_shm = false;
        Env env2(o, false);
        CHECK(Handshake(env2.ep, "z-pref", Transport::kShmPreferred).result() == ENOTSUP);
        ShmServerOptions q = NoRetention();
        q.max_total_bytes = 3ULL << 20; // 只放得下一条共享内存通道
        Env env3(q, false);
        RawConn a = Handshake(env3.ep, "q-a", Transport::kShmRequired);
        CHECK(a.result() == 0);
        CHECK(Handshake(env3.ep, "q-b", Transport::kShmPreferred).result() == ENOSPC);
    }
    { // 共享内存的总量配额满了：优先共享内存的退化成仅 UDS，必须共享内存的拒绝（ENOSPC）。
        ShmServerOptions q = NoRetention();
        q.max_total_bytes = 3ULL << 20;
        Env env(q, true);
        RawConn a = Handshake(env.ep, "q-a", Transport::kShmRequired);
        CHECK(a.result() == 0 && a.shm());
        RawConn b = Handshake(env.ep, "q-b", Transport::kShmPreferred);
        CHECK(b.result() == 0 && b.uds());
        CHECK(Handshake(env.ep, "q-c", Transport::kShmRequired).result() == ENOSPC);
        CHECK(WaitUntil([&] { return env.server->stats().downgraded == 1; }));
    }
    { // 只有"可用性"类的失败才退化：通道数配额（仅 UDS 也占名额）、容量不合法、准入被拒都照常拒绝。
        ShmServerOptions q = NoRetention();
        q.max_channels = 1;
        Env env(q, true);
        RawConn a = Handshake(env.ep, "c-a", Transport::kShmRequired);
        CHECK(a.result() == 0);
        CHECK(Handshake(env.ep, "c-b", Transport::kShmPreferred).result() == ENOSPC);
        CHECK(Handshake(env.ep, "c-c", Transport::kUdsOnly).result() == ENOSPC);

        Env env2(NoRetention(), true);
        CHECK(Handshake(env2.ep, "bad-cap", Transport::kShmPreferred, 0, 3 * shm::kMinRingCapacity).result() == EINVAL);
        env2.handler.admit_deny.store(EACCES);
        CHECK(Handshake(env2.ep, "denied", Transport::kShmPreferred).result() == EACCES);
        CHECK(Handshake(env2.ep, "denied-uds", Transport::kUdsOnly).result() == EACCES);
        const auto admits = env2.handler.admits();
        CHECK(admits.size() >= 2 && admits.back().busid == "denied-uds"); // 仅 UDS 也要过准入回调
        CHECK(env2.server->stats().downgraded == 0 && env2.server->stats().uds_total == 0);
    }
}

// ---- 仅 UDS 的数据通道 ----

void TestUdsOnlyDataPath()
{
    LeakCheck leak;
    Env env(NoRetention(), true);
    EchoOnUds(env);
    std::mutex mu;
    std::vector<UdsPeerContext> seen;
    env.uds_events.on_connected = [&](IConnection& c)
    {
        const UdsPeerContext* ctx = c.GetContext<UdsPeerContext>();
        CHECK(ctx != nullptr); // OnConnected 里就取得到：SetContext 在 Establish 之前
        const std::lock_guard lk(mu);
        UdsPeerContext copy;
        copy.busid = ctx->busid;
        copy.cred = ctx->cred;
        copy.channel_id = ctx->channel_id;
        seen.push_back(std::move(copy));
    };
    RawConn a = Handshake(env.ep, "u-a", Transport::kUdsOnly);
    RawConn b = Handshake(env.ep, "u-b", Transport::kUdsOnly);
    CHECK(a.result() == 0 && b.result() == 0);
    for (int i = 0; i < 200; ++i)
    {
        const std::string msg = "hello-" + std::to_string(i) + std::string(static_cast<std::size_t>(i % 50), 'x');
        SendFrame(a.fd.get(), msg);
        auto got = RecvFrame(a.fd.get());
        CHECK(got.has_value() && *got == msg);
    }
    // 大帧（接近默认上限 64 KiB）与空帧。
    const std::string big(60 * 1024, 'q');
    SendFrame(b.fd.get(), big);
    auto got = RecvFrame(b.fd.get());
    CHECK(got.has_value() && *got == big);
    SendFrame(b.fd.get(), "");
    got = RecvFrame(b.fd.get());
    CHECK(got.has_value() && got->empty());

    CHECK(env.uds_events.WaitMessages(202));
    {
        const std::lock_guard lk(mu);
        CHECK(seen.size() == 2);
        CHECK(seen[0].busid == "u-a" && seen[1].busid == "u-b");
        CHECK(seen[0].channel_id != 0 && seen[0].channel_id != seen[1].channel_id);
        for (const auto& s : seen)
        {
            CHECK(s.cred.uid == ::geteuid() && s.cred.pid == static_cast<std::int32_t>(::getpid()));
        }
    }

    // 超长的帧头：编解码判协议错误，服务端关连接并释放名额与名字。
    const std::uint8_t evil[] = {0x7F, 0xFF, 0xFF, 0xFF};
    RawSend(b.fd.get(), evil, sizeof(evil));
    CHECK(RawSeesEof(b.fd.get()));
    CHECK(WaitUntil([&] { return env.server->stats().uds_channels == 1; }));
    RawConn b2 = Handshake(env.ep, "u-b", Transport::kUdsOnly); // 名字放出来了
    CHECK(b2.result() == 0);
    SendFrame(b2.fd.get(), "again");
    CHECK(RecvFrame(b2.fd.get()).value() == "again");

    // 对端正常关闭：OnClosed(PeerClosed)，登记撤掉。
    a.fd.reset();
    CHECK(WaitUntil([&] { return env.server->stats().uds_channels == 1; }));
    CHECK(WaitUntil([&] { return env.uds_events.closed_count() == 2; }));
    env.uds_events.CheckInvariants();
}

void TestBusIdSharedAcrossTransports()
{
    LeakCheck leak;
    ShmServerOptions o;
    o.retain_default = 30s;
    o.max_channels = 3;
    Env env(o, true);
    { // 共享内存占着的名字，仅 UDS / 共享内存优先的都不能要（优先的不会被"退化"掩盖名字冲突）。
        RawConn s = Handshake(env.ep, "dup1", Transport::kShmRequired);
        CHECK(s.result() == 0);
        CHECK(Handshake(env.ep, "dup1", Transport::kUdsOnly).result() == EBUSY);
        CHECK(Handshake(env.ep, "dup1", Transport::kShmPreferred).result() == EBUSY);
        CHECK(Handshake(env.ep, "dup1", Transport::kShmRequired).result() == EBUSY);
    }
    CHECK(WaitUntil([&] { return env.server->stats().detached_channels == 1; })); // dup1 的 App 走了，保留中
    { // 保留中的共享内存通道：要换成仅 UDS 得带 discard_retained 明说（队列没法交给仅 UDS 的 App）。
        CHECK(Handshake(env.ep, "dup1", Transport::kUdsOnly).result() == EBUSY);
        RawConn u = Handshake(env.ep, "dup1", Transport::kUdsOnly, wire::kFlagDiscardRetained);
        CHECK(u.result() == 0 && u.uds());
        CHECK(WaitUntil([&] { return env.handler.closed_reason("dup1") == ShmCloseReason::Discarded; }));
        CHECK(env.handler.trace("dup1") == "ODC");
        const ShmServerStats st = env.server->stats();
        CHECK(st.discarded == 1 && st.detached_channels == 0 && st.uds_channels == 1);
        // 反过来：仅 UDS 占着的名字，共享内存的来要也是 EBUSY。
        CHECK(Handshake(env.ep, "dup1", Transport::kShmRequired).result() == EBUSY);
        CHECK(Handshake(env.ep, "dup1", Transport::kShmPreferred).result() == EBUSY);
    }
    { // 仅 UDS 对仅 UDS 同名也是 EBUSY。
        RawConn u1 = Handshake(env.ep, "dupu", Transport::kUdsOnly);
        CHECK(u1.result() == 0);
        CHECK(Handshake(env.ep, "dupu", Transport::kUdsOnly).result() == EBUSY);
    }
    CHECK(WaitUntil([&] { return env.server->stats().uds_channels == 0; })); // u、u1 出作用域关了
    // 名字放出来之后任何传输都能要；通道数配额（3）两种传输一起算。
    RawConn x1 = Handshake(env.ep, "dup1", Transport::kShmRequired);
    RawConn x2 = Handshake(env.ep, "q2", Transport::kUdsOnly);
    RawConn x3 = Handshake(env.ep, "q3", Transport::kUdsOnly);
    CHECK(x1.result() == 0 && x2.result() == 0 && x3.result() == 0);
    CHECK(Handshake(env.ep, "q4", Transport::kUdsOnly).result() == ENOSPC);
    CHECK(Handshake(env.ep, "q4", Transport::kShmRequired).result() == ENOSPC);
    x2.fd.reset();
    CHECK(WaitUntil([&] { return env.server->stats().uds_channels == 1; }));
    CHECK(Handshake(env.ep, "q4", Transport::kUdsOnly).result() == 0); // 名额还回来了
}

// 保留着的共享内存通道只还给同一个用户：别的用户带 discard_retained 想把它换成仅 UDS 也不行（不然谁都能毁掉别人的队列）。
void TestUdsOnlyDiscardIdentity()
{
    if (::geteuid() != 0)
    {
        std::puts("TestUdsOnlyDiscardIdentity: 跳过（要 root 才能换用户）");
        return;
    }
    LeakCheck leak;
    ShmServerOptions o;
    o.retain_default = 30s;
    Env env(o, true);
    OpenSocketToAll(env);
    {
        RawConn s = Handshake(env.ep, "idt", Transport::kShmRequired);
        CHECK(s.result() == 0);
    }
    CHECK(WaitUntil([&] { return env.server->stats().detached_channels == 1; }));
    const int e = AsUser(65534,
                         [&]
                         {
                             return Handshake(env.ep, "idt", Transport::kUdsOnly, wire::kFlagDiscardRetained).result();
                         });
    CHECK(e == EACCES);
    CHECK(env.server->stats().detached_channels == 1 && env.server->stats().discarded == 0);
    // 本人可以。
    CHECK(Handshake(env.ep, "idt", Transport::kUdsOnly, wire::kFlagDiscardRetained).result() == 0);
}

void TestUdsOnlyHandshakeAbuse()
{
    LeakCheck leak;
    Env env(NoRetention(), true);
    { // 仅 UDS 的 Hello 不该带 fd。
        int p[2];
        CHECK(::pipe2(p, O_CLOEXEC) == 0);
        UniqueFd rd(p[0]), wr(p[1]);
        UniqueFd fd = RawConnect(env.ep);
        wire::Hello h = MakeHello("u-fds");
        h.transport = static_cast<std::uint32_t>(Transport::kUdsOnly);
        const std::array<int, 1> fds{p[1]};
        CHECK(sock::SendWithFds(fd.get(), std::as_bytes(std::span(&h, 1)), fds) == 0);
        auto r = RawRecvReply(fd.get());
        CHECK(r.has_value() && r->reply.result == EPROTO && r->nfds == 0);
        CHECK(env.server->stats().uds_total == 0);
    }
    { // Hello 之后紧跟着的数据（同一次 write 里）：属于数据通道，不能被握手吞掉。
        UniqueFd fd = RawConnect(env.ep);
        wire::Hello h = MakeHello("u-tail");
        h.transport = static_cast<std::uint32_t>(Transport::kUdsOnly);
        std::string both(reinterpret_cast<const char*>(&h), sizeof(h));
        const std::string payload = "tail";
        const std::uint8_t len[4] = {0, 0, 0, 4};
        both.append(reinterpret_cast<const char*>(len), 4);
        both.append(payload);
        EchoOnUds(env);
        RawSend(fd.get(), both.data(), both.size());
        auto r = RawRecvReply(fd.get());
        CHECK(r.has_value() && r->reply.result == 0);
        // 这条帧是和 Hello 一起到的：服务端握手只读 Hello 那么多字节，剩下的留在 socket 里给数据通道。
        CHECK(RecvFrame(fd.get()).value_or("") == "tail");
    }
}

void TestStopClosesUdsPeers()
{
    LeakCheck leak;
    Env env(NoRetention(), true);
    RawConn a = Handshake(env.ep, "s-a", Transport::kUdsOnly);
    RawConn b = Handshake(env.ep, "s-b", Transport::kShmRequired);
    CHECK(a.result() == 0 && b.result() == 0);
    CHECK(WaitUntil([&] { return env.server->stats().open_channels == 2; }));
    env.server->Stop();
    CHECK(RawSeesEof(a.fd.get()));
    CHECK(WaitUntil([&] { return env.uds_events.closed_count() == 1; }));
    CHECK(WaitUntil([&] { return env.handler.closed_count() == 1; }));
    CHECK(WaitUntil([&] { return env.server->stats().open_channels == 0; }));
    env.uds_events.CheckInvariants();
}

// 服务端先于仅 UDS 的连接销毁（连接要等一轮才回收）：不能留下对服务端内部状态的引用。asan 下才测得出。
void TestServerDestroyedWithLiveUdsPeers()
{
    LeakCheck leak;
    for (int round = 0; round < 20; ++round)
    {
        std::vector<RawConn> peers;
        {
            Env env(NoRetention(), true);
            EchoOnUds(env);
            for (int i = 0; i < 5; ++i)
                peers.push_back(Handshake(env.ep, "d" + std::to_string(i), Transport::kUdsOnly));
            for (auto& p : peers)
            {
                CHECK(p.result() == 0);
                SendFrame(p.fd.get(), "x");
                CHECK(RecvFrame(p.fd.get()).value_or("") == "x"); // 回显读掉，之后 socket 上只剩 EOF
            }
            CHECK(env.uds_events.WaitMessages(5));
        } // Env 在这里销毁：服务端、group、连接
        for (auto& p : peers)
            CHECK(RawSeesEof(p.fd.get()));
    }
}

// ~ShmChannelServer 返回之后，这个服务端的仅 UDS 连接不会再有任何回调（OnClosed 要等连接表的回收阶段，析构要等它们都回收完）。
// 处理器放在堆上、服务端一销毁就释放，而 group 还活着继续跑：要是还有回调碰它，asan 会报堆释放后使用。
void TestNoCallbacksAfterServerDestroyed()
{
    LeakCheck leak;
    for (int round = 0; round < 5; ++round)
    {
        TempDir dir;
        const Endpoint ep = dir.Unix("agent.sock");
        EventLoopGroup group(EventLoopGroupOptions{1, 256});
        CHECK(group.Start() == 0);
        EchoHandler shm_handler;
        auto recorder = std::make_unique<EventRecorder>();
        // OnClosed 里睡一会儿：把"服务端析构返回时回调还在进行"这个窗口撑开。析构不等连接回收完的话，处理器随即被释放，
        // 回调醒来就碰到已释放的东西（asan 报堆释放后使用）。
        recorder->on_closed = [](IConnection&, CloseReason) { std::this_thread::sleep_for(30ms); };
        auto codec = std::make_unique<LengthPrefixCodecFactory>();
        ShmUdsDataPath path;
        path.handler = recorder.get();
        path.factory = codec.get();
        auto server = std::make_unique<ShmChannelServer>(group, ep, NoRetention(), shm_handler, path);
        CHECK(server->Start() == 0);
        std::vector<RawConn> peers;
        for (int i = 0; i < 5; ++i)
            peers.push_back(Handshake(ep, "k" + std::to_string(i), Transport::kUdsOnly));
        CHECK(WaitUntil([&] { return recorder->connected_count() == 5; }));
        server.reset();   // 同步销毁：此后不许再有回调
        recorder.reset(); // 处理器随即释放（group 还在跑）
        codec.reset();
        for (auto& p : peers)
            CHECK(RawSeesEof(p.fd.get()));
        std::this_thread::sleep_for(20ms); // 给 loop 留出回收的时间：有残留回调的话在这段时间里碰到已释放的处理器
    }
}

} // namespace

#define RUN(t)                                                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        std::fprintf(stderr, "[run] %s\n", #t);                                                                        \
        t();                                                                                                           \
    } while (0)

int main()
{
    RUN(TestNegotiationMatrix);
    RUN(TestUdsOnlyDataPath);
    RUN(TestBusIdSharedAcrossTransports);
    RUN(TestUdsOnlyDiscardIdentity);
    RUN(TestUdsOnlyHandshakeAbuse);
    RUN(TestStopClosesUdsPeers);
    RUN(TestServerDestroyedWithLiveUdsPeers);
    RUN(TestNoCallbacksAfterServerDestroyed);
    std::puts("shm_transport_test: OK");
    return 0;
}
