// KcpSession：消息边界、分段、空消息、上限、丢包重传、乱序、重复、链路判死、确认计数。
// 两个会话用内存里的"线"连起来，时钟由测试推进（Update / Input 都传 now），不睡觉、不开 socket，结果可复现。
// 无测试框架依赖：失败即 abort。

#include "kcp_session.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

using namespace breeze;

#define CHECK(...)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #__VA_ARGS__);                       \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

namespace
{

using Datagram = std::vector<std::byte>;

std::vector<std::byte> Bytes(const std::string& s)
{
    return {reinterpret_cast<const std::byte*>(s.data()), reinterpret_cast<const std::byte*>(s.data()) + s.size()};
}

// 一端：KCP 会话 + 它发出的数据报（output 回调里收集）。
struct Peer
{
    std::unique_ptr<KcpSession> kcp;
    std::vector<Datagram> outbox;
    std::size_t outputs = 0;               // 累计 output 次数（含被丢的）
    std::function<bool(std::size_t)> lose; // 第 n 个输出要不要丢；空 = 不丢
};

void Output(void* user, std::span<const std::byte> data)
{
    auto* p = static_cast<Peer*>(user);
    const std::size_t n = p->outputs++;
    if (p->lose && p->lose(n))
        return;
    p->outbox.emplace_back(data.begin(), data.end());
}

struct Pair
{
    Peer a, b;
    std::uint32_t now = 100000;
    bool reorder = false;   // 每一步里把数据报倒过来送
    bool duplicate = false; // 每个数据报送两遍

    explicit Pair(const RUdpOptions& opt = {}, std::uint32_t conv_a = 7, std::uint32_t conv_b = 7)
    {
        a.kcp = KcpSession::Create(conv_a, opt, &Output, &a);
        b.kcp = KcpSession::Create(conv_b, opt, &Output, &b);
        CHECK(a.kcp && b.kcp);
        a.kcp->Update(now);
        b.kcp->Update(now);
    }

    static void Deliver(Peer& from, KcpSession& to, std::uint32_t now, bool reorder, bool duplicate)
    {
        std::vector<Datagram> batch;
        batch.swap(from.outbox);
        if (reorder)
            std::reverse(batch.begin(), batch.end());
        for (const auto& d : batch)
        {
            (void)to.Input(d, now);
            if (duplicate)
                (void)to.Input(d, now);
        }
    }

    // 推进 ms 毫秒（10ms 一步）：两端 Update，再把各自发出的数据报送给对端。
    void Run(std::uint32_t ms)
    {
        for (std::uint32_t t = 0; t < ms; t += 10)
        {
            now += 10;
            a.kcp->Update(now);
            b.kcp->Update(now);
            Deliver(a, *b.kcp, now, reorder, duplicate);
            Deliver(b, *a.kcp, now, reorder, duplicate);
        }
    }
};

std::vector<std::string> Drain(KcpSession& k)
{
    std::vector<std::string> out;
    for (;;)
    {
        const int size = k.PeekSize();
        if (size < 0)
            return out;
        std::vector<std::byte> buf(static_cast<std::size_t>(size));
        const int got = k.Recv(buf);
        CHECK(got == size);
        out.emplace_back(reinterpret_cast<const char*>(buf.data()), buf.size());
    }
}

void SendStr(KcpSession& k, const std::string& s)
{
    std::uint32_t frags = 0;
    CHECK(k.Send(Bytes(s), frags) == 0);
}

// 消息保持边界：三次 Send 对端三次 Recv，各自完整（不像字节流会粘包）。
void TestMessageBoundaries()
{
    Pair p;
    SendStr(*p.a.kcp, "a");
    SendStr(*p.a.kcp, "bb");
    SendStr(*p.a.kcp, "ccc");
    p.Run(100);
    CHECK(Drain(*p.b.kcp) == (std::vector<std::string>{"a", "bb", "ccc"}));
    CHECK(p.b.kcp->PeekSize() == -1); // 取空了
}

// 双向：两端各发各的，互不影响。
void TestBidirectional()
{
    Pair p;
    SendStr(*p.a.kcp, "from-a");
    SendStr(*p.b.kcp, "from-b");
    p.Run(100);
    CHECK(Drain(*p.b.kcp) == (std::vector<std::string>{"from-a"}));
    CHECK(Drain(*p.a.kcp) == (std::vector<std::string>{"from-b"}));
}

// 每段能装 mtu - 1（rudp 包头）- 24（KCP 头）字节。
void TestMss()
{
    RUdpOptions o;
    o.mtu = 1400;
    Pair p(o);
    CHECK(p.a.kcp->mss() == 1400 - 1 - 24);
    o.mtu = 600;
    Pair q(o);
    CHECK(q.a.kcp->mss() == 600 - 1 - 24);
}

// 占几段：0 字节也占一段；刚好一段；多一个字节两段。
void TestFragmentCount()
{
    Pair p;
    const std::uint32_t mss = p.a.kcp->mss();
    std::uint32_t frags = 99;
    CHECK(p.a.kcp->Send(Bytes(""), frags) == 0 && frags == 1);
    CHECK(p.a.kcp->Send(Bytes(std::string(mss, 'x')), frags) == 0 && frags == 1);
    CHECK(p.a.kcp->Send(Bytes(std::string(mss + 1, 'x')), frags) == 0 && frags == 2);
    CHECK(p.a.kcp->Send(Bytes(std::string(3 * mss, 'x')), frags) == 0 && frags == 3);
    CHECK(p.a.kcp->Send(Bytes(std::string(3 * mss + 1, 'x')), frags) == 0 && frags == 4);
}

// 空消息是合法消息：对端收到长度 0 的一条，不是"没有消息"。
void TestEmptyMessage()
{
    Pair p;
    SendStr(*p.a.kcp, "");
    SendStr(*p.a.kcp, "after");
    p.Run(100);
    CHECK(Drain(*p.b.kcp) == (std::vector<std::string>{"", "after"}));
}

// 大消息被分段、对端收齐才交付一次；分段没收齐之前 PeekSize 是 -1。
void TestLargeMessage()
{
    RUdpOptions o;
    o.recv_window = 256;
    Pair p(o);
    std::string big(60000, '\0');
    for (std::size_t i = 0; i < big.size(); ++i)
        big[i] = static_cast<char>('a' + i % 26);
    SendStr(*p.a.kcp, big);
    SendStr(*p.a.kcp, "tail");
    p.Run(500);
    const auto got = Drain(*p.b.kcp);
    CHECK(got.size() == 2 && got[0] == big && got[1] == "tail");
}

// 上限：127 段能发，128 段不行（ikcp 的限制）；MaxMessageForMtu 与它对得上。
void TestMaxMessage()
{
    RUdpOptions o;
    Pair p(o);
    const std::size_t max = rudp::MaxMessageForMtu(o.mtu);
    CHECK(max == 127u * p.a.kcp->mss());
    std::uint32_t frags = 0;
    CHECK(p.a.kcp->Send(Bytes(std::string(max, 'x')), frags) == 0 && frags == 127);
    CHECK(p.a.kcp->Send(Bytes(std::string(max + 1, 'x')), frags) < 0);
}

// 最大的一条消息真能端到端送达（窗口要装得下 127 段）。
void TestMaxMessageDelivered()
{
    RUdpOptions o;
    Pair p(o);
    const std::size_t max = rudp::MaxMessageForMtu(o.mtu);
    std::string m(max, 'm');
    SendStr(*p.a.kcp, m);
    p.Run(2000);
    const auto got = Drain(*p.b.kcp);
    CHECK(got.size() == 1 && got[0] == m);
}

// 丢包：前几个数据报全丢，靠重传仍然按序送达。
void TestLossRetransmit()
{
    Pair p;
    p.a.lose = [](std::size_t n) { return n < 3; }; // 头 3 个输出丢掉
    for (int i = 0; i < 10; ++i)
        SendStr(*p.a.kcp, "m" + std::to_string(i));
    p.Run(5000);
    std::vector<std::string> want;
    for (int i = 0; i < 10; ++i)
        want.push_back("m" + std::to_string(i));
    CHECK(Drain(*p.b.kcp) == want);
    CHECK(p.a.kcp->WaitSnd() == 0); // 都确认了
}

// 持续的随机丢包（30%，确定性序列）：最终全部按序送达。
void TestHeavyLoss()
{
    Pair p;
    std::uint32_t x = 12345;
    auto lossy = [x](std::size_t) mutable
    {
        x = x * 1664525u + 1013904223u;
        return (x >> 16) % 100 < 30;
    };
    p.a.lose = lossy;
    p.b.lose = lossy;
    std::vector<std::string> want;
    for (int i = 0; i < 200; ++i)
    {
        want.push_back("msg-" + std::to_string(i));
        SendStr(*p.a.kcp, want.back());
    }
    p.Run(60000);
    CHECK(Drain(*p.b.kcp) == want);
}

// 乱序：同一步里的数据报倒着到，仍按序交付。
void TestReorder()
{
    Pair p;
    p.reorder = true;
    std::vector<std::string> want;
    for (int i = 0; i < 50; ++i)
    {
        want.push_back("r" + std::to_string(i));
        SendStr(*p.a.kcp, want.back());
    }
    p.Run(3000);
    CHECK(Drain(*p.b.kcp) == want);
}

// 重复：每个数据报到两遍，消息只交付一次。
void TestDuplicates()
{
    Pair p;
    p.duplicate = true;
    for (int i = 0; i < 20; ++i)
        SendStr(*p.a.kcp, "d" + std::to_string(i));
    p.Run(1000);
    CHECK(Drain(*p.b.kcp).size() == 20);
}

// 发送窗口远小于消息数：排队等窗口，最终全部送达（不丢、不乱序）。
void TestSendQueueBeyondWindow()
{
    RUdpOptions o;
    o.send_window = 4;
    Pair p(o);
    std::vector<std::string> want;
    for (int i = 0; i < 100; ++i)
    {
        want.push_back("w" + std::to_string(i));
        SendStr(*p.a.kcp, want.back());
    }
    CHECK(p.a.kcp->WaitSnd() == 100); // 都排着，没有一个被确认
    p.Run(5000);
    CHECK(Drain(*p.b.kcp) == want);
    CHECK(p.a.kcp->WaitSnd() == 0);
}

// WaitSnd：Send 之后立刻 > 0（还没确认），完整往返之后回到 0。
void TestWaitSnd()
{
    Pair p;
    CHECK(p.a.kcp->WaitSnd() == 0);
    SendStr(*p.a.kcp, "x");
    SendStr(*p.a.kcp, "y");
    CHECK(p.a.kcp->WaitSnd() == 2);
    p.Run(200);
    CHECK(p.a.kcp->WaitSnd() == 0);
}

// 没有对端回应：同一个段重传 dead_link 次之后判死。
void TestDeadLink()
{
    RUdpOptions o;
    o.dead_link = 5;
    Pair p(o);
    p.b.lose = [](std::size_t) { return true; }; // 对端的 ack 全丢
    p.a.lose = [](std::size_t) { return true; }; // 数据也到不了
    CHECK(!p.a.kcp->Dead());
    SendStr(*p.a.kcp, "x");
    // 5 次传输约 1.7 秒（RTO 200 → 300 → 450 → 675ms）：30 秒内必须判死。
    // 默认的 20 次要几分钟，没把 dead_link 传进去就过不了
    for (int i = 0; i < 30 && !p.a.kcp->Dead(); ++i)
        p.Run(1000);
    CHECK(p.a.kcp->Dead());
}

// 正常通信不会判死（哪怕丢一些包）。
void TestNotDeadWhenAlive()
{
    Pair p;
    p.a.lose = [](std::size_t n) { return n % 3 == 0; };
    for (int i = 0; i < 20; ++i)
        SendStr(*p.a.kcp, "k" + std::to_string(i));
    p.Run(10000);
    CHECK(!p.a.kcp->Dead() && !p.b.kcp->Dead());
}

// conv 不符 / 垃圾：Input 返回负数，不影响会话。
void TestInputRejectsForeign()
{
    Pair p(RUdpOptions{}, 7, 8); // 两端 conv 不同：互相不认
    SendStr(*p.a.kcp, "hello");
    p.Run(200);
    CHECK(Drain(*p.b.kcp).empty());

    Pair q;
    const Datagram junk(30, std::byte{0x5A});
    CHECK(q.b.kcp->Input(junk, q.now) < 0);
    const Datagram tiny(3, std::byte{0});
    CHECK(q.b.kcp->Input(tiny, q.now) < 0);
    // 垃圾之后会话还能正常工作
    SendStr(*q.a.kcp, "ok");
    q.Run(100);
    CHECK(Drain(*q.b.kcp) == (std::vector<std::string>{"ok"}));
}

// Flush：不等 Update 的节拍，立刻把排队的数据发出去。
void TestFlushImmediate()
{
    Pair p;
    p.a.outbox.clear();
    SendStr(*p.a.kcp, "now");
    CHECK(p.a.outbox.empty()); // Send 只排队
    p.a.kcp->Flush(p.now);
    CHECK(!p.a.outbox.empty());
}

// 输出的数据报不超过 mtu - 1（rudp 包头要再加 1 字节）；大消息把它们填满，不是碎包。
// 只推进 a 的时钟、不把数据报送走，这样能看到它发出的全部。
void TestDatagramSizeBound()
{
    RUdpOptions o;
    o.mtu = 800;
    Pair p(o);
    SendStr(*p.a.kcp, std::string(50000, 'z'));
    for (std::uint32_t t = 0; t < 2000; t += 10)
    {
        p.now += 10;
        p.a.kcp->Update(p.now);
    }
    CHECK(!p.a.outbox.empty());
    std::size_t max_seen = 0;
    for (const auto& d : p.a.outbox)
        max_seen = std::max(max_seen, d.size());
    CHECK(max_seen <= 800 - 1);
    CHECK(max_seen > 700);
}

// NeedsUpdate：新建 / 完整往返之后是静止的；有没发完或没被确认的数据、有待发的确认、有待发的窗口通告、
// 对端窗口为 0 时都为真。Update 不会白走：静止时可以不调。
void TestNeedsUpdate()
{
    {
        Pair p;
        CHECK(!p.a.kcp->NeedsUpdate() && !p.b.kcp->NeedsUpdate()); // 新建：静止
        SendStr(*p.a.kcp, "x");
        CHECK(p.a.kcp->NeedsUpdate()); // 排着队的数据
        p.Run(300);
        CHECK(!p.a.kcp->NeedsUpdate() && !p.b.kcp->NeedsUpdate()); // 一个完整往返之后又静止了
        CHECK(Drain(*p.b.kcp).size() == 1);
    }
    {
        // 收到数据、还没 flush：有待发的确认
        Pair p;
        SendStr(*p.a.kcp, "y");
        p.now += 10;
        p.a.kcp->Update(p.now);
        Pair::Deliver(p.a, *p.b.kcp, p.now, false, false);
        CHECK(p.b.kcp->NeedsUpdate());
        p.b.kcp->Update(p.now); // 确认发出去
        CHECK(!p.b.kcp->NeedsUpdate());
    }
    {
        // 数据一直没被确认：一直要 Update（重传）
        Pair p;
        p.a.lose = [](std::size_t) { return true; };
        SendStr(*p.a.kcp, "z");
        p.Run(2000);
        CHECK(p.a.kcp->NeedsUpdate());
    }
    {
        // 对端窗口为 0：即使什么都不用发也要 Update（定时探测）；对端读走消息、窗口恢复后，
        // 它有一条窗口通告要发，发完两边都静止
        RUdpOptions o;
        o.recv_window = 128; // KCP 的下限；发满 128 条、对端又不读，它的接收窗口就是 0
        Pair p(o);
        for (int i = 0; i < 128; ++i)
            SendStr(*p.a.kcp, "w" + std::to_string(i));
        p.Run(1500);
        CHECK(p.a.kcp->WaitSnd() == 0); // 都确认了
        CHECK(p.a.kcp->NeedsUpdate());  // 只因为对端窗口是 0
        CHECK(Drain(*p.b.kcp).size() == 128);
        CHECK(p.b.kcp->NeedsUpdate()); // 窗口恢复了，要通告
        p.Run(300);
        CHECK(!p.a.kcp->NeedsUpdate() && !p.b.kcp->NeedsUpdate());
    }
}

using Kind = UpdatePlan::Kind;

// PlanUpdate：排队的数据 → Soon；在途的段 → At（最早的重传时刻，到点就是 Soon）；收到数据后有待发的确认 → Soon；
// 确认回来、什么都不剩 → Quiescent；对端窗口为 0 → 先 Soon（起探测计时）再 At（5 秒后的探测）。
// 并且始终与 NeedsUpdate 一致：Quiescent 当且仅当 !NeedsUpdate()。
void TestPlanUpdate()
{
    {
        Pair p;
        CHECK(p.a.kcp->PlanUpdate(p.now).kind == Kind::Quiescent);
        SendStr(*p.a.kcp, "x");
        CHECK(p.a.kcp->PlanUpdate(p.now).kind == Kind::Soon); // 排队的数据，下一拍就 flush
        p.now += 10;
        p.a.kcp->Update(p.now); // 发出去：在途，没有别的事
        const UpdatePlan plan = p.a.kcp->PlanUpdate(p.now);
        CHECK(plan.kind == Kind::At);
        CHECK(plan.at - p.now >= 30 && plan.at - p.now <= 400);   // 最早的重传时刻在将来（初始 RTO 200ms 量级）
        CHECK(p.a.kcp->PlanUpdate(plan.at - 1).kind == Kind::At); // 差 1ms：还没到
        CHECK(p.a.kcp->PlanUpdate(plan.at).kind == Kind::Soon);   // 正好到点：要重传了
        CHECK(p.a.kcp->PlanUpdate(plan.at + 5).kind == Kind::Soon);

        // 对端收到：有待发的确认；它 flush 之后静止；确认回来，a 也静止
        Pair::Deliver(p.a, *p.b.kcp, p.now, false, false);
        CHECK(p.b.kcp->PlanUpdate(p.now).kind == Kind::Soon);
        p.b.kcp->Update(p.now);
        CHECK(p.b.kcp->PlanUpdate(p.now).kind == Kind::Quiescent);
        Pair::Deliver(p.b, *p.a.kcp, p.now, false, false);
        CHECK(p.a.kcp->PlanUpdate(p.now).kind == Kind::Quiescent);
    }
    {
        // 待发的窗口通告：ikcp_update 受 ts_flush 限制，调早了不 flush，通告还留着——计划必须还是 Soon，
        // 不能因为"没有排队的数据、没有在途的段"就睡过去（对端的窗口一直是 0，要等 5 秒的探测才会再问）
        RUdpOptions o;
        o.recv_window = 128;
        Pair p(o);
        for (int i = 0; i < 128; ++i)
            SendStr(*p.a.kcp, "t" + std::to_string(i));
        p.Run(1500);
        CHECK(Drain(*p.b.kcp).size() == 128); // 读走消息：b 要通告窗口恢复了
        CHECK(p.b.kcp->PlanUpdate(p.now).kind == Kind::Soon);
        p.b.kcp->Update(p.now + 1); // 在 ts_flush 之前调：不会 flush
        CHECK(p.b.kcp->PlanUpdate(p.now + 1).kind == Kind::Soon);
    }
    {
        // 多个在途的段：取最早的重传时刻（先发的先到期），后发的不能把它推迟
        Pair p;
        SendStr(*p.a.kcp, "first");
        p.now += 10;
        p.a.kcp->Update(p.now);
        const UpdatePlan first = p.a.kcp->PlanUpdate(p.now);
        CHECK(first.kind == Kind::At);
        SendStr(*p.a.kcp, "second");
        p.now += 50;
        p.a.kcp->Update(p.now); // 第二个段在 50ms 之后发出，它的重传时刻更晚
        CHECK(p.a.kcp->WaitSnd() == 2);
        const UpdatePlan both = p.a.kcp->PlanUpdate(p.now);
        CHECK(both.kind == Kind::At && both.at == first.at);
    }
    {
        // 对端窗口为 0
        RUdpOptions o;
        o.recv_window = 128;
        Pair p(o);
        for (int i = 0; i < 128; ++i)
            SendStr(*p.a.kcp, "w" + std::to_string(i));
        p.Run(1500);
        CHECK(p.a.kcp->WaitSnd() == 0);
        const UpdatePlan plan = p.a.kcp->PlanUpdate(p.now);
        CHECK(plan.kind == Kind::At); // 探测计时已经在 Run 里起来了：下一次探测在约 5 秒之后
        CHECK(plan.at - p.now > 1000 && plan.at - p.now <= 6000);
        CHECK(p.a.kcp->PlanUpdate(plan.at).kind == Kind::Soon);
    }
    {
        // 与 NeedsUpdate 一致：丢包的链路上每一步都对一遍
        Pair p;
        std::uint32_t x = 777;
        p.a.lose = [x](std::size_t) mutable
        {
            x = x * 1664525u + 1013904223u;
            return (x >> 16) % 100 < 40;
        };
        for (int i = 0; i < 30; ++i)
            SendStr(*p.a.kcp, "c" + std::to_string(i));
        for (int step = 0; step < 600; ++step)
        {
            p.Run(10);
            CHECK((p.a.kcp->PlanUpdate(p.now).kind == Kind::Quiescent) == !p.a.kcp->NeedsUpdate());
            CHECK((p.b.kcp->PlanUpdate(p.now).kind == Kind::Quiescent) == !p.b.kcp->NeedsUpdate());
        }
    }
}

// 按 PlanUpdate 调度（只在计划到点、或被 Input / Send 唤醒时才 Update）的会话，在 30% 丢包 + 乱序下照样把消息
// 全部按序送达，而且 Update 的次数远少于"每拍都调"——说明计划既没睡过重传时刻，也确实省了调用。
void TestPlannedSchedulingStillDelivers()
{
    struct Side
    {
        bool immediate = true;
        bool asleep = false;
        std::uint32_t due = 0;
        int updates = 0;
    };
    Pair p;
    std::uint32_t x = 4242;
    const auto lossy = [x](std::size_t) mutable
    {
        x = x * 1664525u + 1013904223u;
        return (x >> 16) % 100 < 30;
    };
    p.a.lose = lossy;
    p.b.lose = lossy;
    p.reorder = true;
    Side sa, sb;
    const auto wake = [](Side& s)
    {
        s.immediate = true;
        s.asleep = false;
    };
    const auto tick = [&](Peer& peer, Side& s)
    {
        if (s.asleep || (!s.immediate && static_cast<std::int32_t>(p.now - s.due) < 0))
            return;
        peer.kcp->Update(p.now);
        ++s.updates;
        const UpdatePlan plan = peer.kcp->PlanUpdate(p.now);
        s.immediate = plan.kind == Kind::Soon;
        s.asleep = plan.kind == Kind::Quiescent;
        s.due = plan.at;
    };
    std::vector<std::string> want;
    for (int i = 0; i < 100; ++i)
    {
        want.push_back("p" + std::to_string(i));
        SendStr(*p.a.kcp, want.back());
    }
    wake(sa);
    std::vector<std::string> got;
    int steps = 0;
    for (; steps < 3000 && got.size() < want.size(); ++steps) // 最多 30 秒（假时钟）
    {
        p.now += 10;
        tick(p.a, sa);
        tick(p.b, sb);
        // 送包：谁收到包谁被唤醒（Pair::Deliver 的逻辑，加上唤醒）
        if (!p.a.outbox.empty())
            wake(sb);
        if (!p.b.outbox.empty())
            wake(sa);
        Pair::Deliver(p.a, *p.b.kcp, p.now, p.reorder, false);
        Pair::Deliver(p.b, *p.a.kcp, p.now, p.reorder, false);
        for (auto& m : Drain(*p.b.kcp))
            got.push_back(std::move(m));
    }
    CHECK(got == want);
    // 收齐之后再跑一阵，让最后的确认也走完
    for (int i = 0; i < 300; ++i)
    {
        p.now += 10;
        tick(p.a, sa);
        tick(p.b, sb);
        if (!p.a.outbox.empty())
            wake(sb);
        if (!p.b.outbox.empty())
            wake(sa);
        Pair::Deliver(p.a, *p.b.kcp, p.now, p.reorder, false);
        Pair::Deliver(p.b, *p.a.kcp, p.now, p.reorder, false);
    }
    const int naive = 2 * (steps + 300); // 每拍每个会话都调一次
    CHECK(sa.updates + sb.updates < naive / 2);
    CHECK(p.a.kcp->WaitSnd() == 0); // 都确认了
}

// 静止了很久的会话（没人 Update，KCP 的时钟停在很久以前）再发一条并立刻 flush：段的时间戳与重传时刻必须按"现在"算。
// 否则 resendts = 很久以前 + RTO，已经过去了，下一拍就白白重传一次；对端确认时算出的 RTT 也包含这整段静止的时间，
// RTO 被拉到几十秒，之后真丢包了要等很久。
void TestFlushUsesTheCurrentClock()
{
    Pair p;
    p.Run(100);
    p.now += 60000; // 静止了一分钟，没有人调 Update / Input
    SendStr(*p.a.kcp, "late");
    p.a.kcp->Flush(p.now);
    const UpdatePlan plan = p.a.kcp->PlanUpdate(p.now);
    CHECK(plan.kind == Kind::At); // 重传时刻在将来，不是"已经过去了"
    CHECK(plan.at - p.now >= 30 && plan.at - p.now <= 400);
}

void TestNowMs()
{
    const std::uint32_t a = KcpNowMs();
    const std::uint32_t b = KcpNowMs();
    CHECK(static_cast<std::int32_t>(b - a) >= 0 && b - a < 1000);
}

} // namespace

int main()
{
    TestMessageBoundaries();
    TestBidirectional();
    TestMss();
    TestFragmentCount();
    TestEmptyMessage();
    TestLargeMessage();
    TestMaxMessage();
    TestMaxMessageDelivered();
    TestLossRetransmit();
    TestHeavyLoss();
    TestReorder();
    TestDuplicates();
    TestSendQueueBeyondWindow();
    TestWaitSnd();
    TestDeadLink();
    TestNotDeadWhenAlive();
    TestInputRejectsForeign();
    TestFlushImmediate();
    TestNeedsUpdate();
    TestPlanUpdate();
    TestPlannedSchedulingStillDelivers();
    TestFlushUsesTheCurrentClock();
    TestDatagramSizeBound();
    TestNowMs();
    std::puts("kcp_session_test: ok");
    return 0;
}
