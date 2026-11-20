// ConnectionHandle：位宽打包、跨线程 Send / Close、旧句柄不串到复用同一槽的新连接、各种无效句柄、
// group 销毁后的句柄、慢消费者（水位）与 pending_bytes 记账、Send 与 Close 并发（tsan 下跑）、
// 批内合并（同一批里相邻的同连接 Send 合成一次 sendmsg）的顺序 / 记账 / 部分写入 / 与 Close 的相对顺序。
//
// 这一阶段连接仍由单 loop 的 LoopServer 创建（挂在 group 的 loop 0 上）；StreamServer 之后换掉它。

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "base/net/codec/length_prefix_codec.h"
#include "base/net/connection/connection_interface.h"
#include "base/net/loop/event_loop_group.h"
#include "base/net/loop/group_access.h"
#include "base/net/stream/internal/loop_server.h"
#include "base/net/testing/net_test_util.h"

using namespace breeze;
using namespace breeze::testing;

namespace
{

// 记录每条连接的句柄与关闭原因；默认不回显（要回显的用例自己开）。
class Recorder final : public IConnectionHandler
{
public:
    void OnConnected(IConnection& c) noexcept override
    {
        const std::lock_guard lk(mu_);
        handles_.push_back(c.handle());
    }
    void OnMessage(IConnection&, std::span<const std::byte>) noexcept override {}
    void OnClosed(IConnection& c, CloseReason r, int) noexcept override
    {
        const std::lock_guard lk(mu_);
        closed_.emplace_back(c.handle(), r);
    }

    ConnectionHandle WaitHandle(std::size_t i)
    {
        CHECK(WaitUntil(
            [&]
            {
                const std::lock_guard lk(mu_);
                return handles_.size() > i;
            }));
        const std::lock_guard lk(mu_);
        return handles_[i];
    }

    std::vector<std::pair<ConnectionHandle, CloseReason>> WaitClosed(std::size_t n)
    {
        CHECK(WaitUntil([&] { return ClosedCount() >= n; }));
        const std::lock_guard lk(mu_);
        return closed_;
    }

    std::size_t ClosedCount() const
    {
        const std::lock_guard lk(mu_);
        return closed_.size();
    }

private:
    mutable std::mutex mu_;
    std::vector<ConnectionHandle> handles_;
    std::vector<std::pair<ConnectionHandle, CloseReason>> closed_;
};

// 一个 group + 一个挂在 loop 0 上的 LoopServer。
class GroupServer
{
public:
    explicit GroupServer(IConnectionHandler& h, EventLoopGroupOptions go = {1, 64}, ConnectionOptions co = {})
        : group(go)
        , factory(4 * 1024 * 1024)
    {
        CHECK(group.Start() == 0);
        srv = std::make_unique<LoopServer>(loop(), *Endpoint::Tcp("127.0.0.1", 0), h, factory, co);
        int rc = -1;
        RunOnLoop(loop(), [&] { rc = srv->Start(); });
        CHECK(rc == 0);
    }
    ~GroupServer() { Stop(); }

    // Acceptor 只能在 loop 线程上停；之后才能停 group。
    void Stop()
    {
        if (stopped_)
            return;
        stopped_ = true;
        RunOnLoop(loop(), [&] { srv->Stop(); });
        group.Shutdown(0ms);
    }

    EventLoop& loop() { return GroupAccess::LoopAt(group, 0); }
    const Endpoint& ep() const { return srv->endpoint(); }
    bool Drained() { return loop().table().Live() == 0; }

    EventLoopGroup group;
    LengthPrefixCodecFactory factory;
    std::unique_ptr<LoopServer> srv;

private:
    bool stopped_ = false;
};

void TestHandlePacking()
{
    const auto h = ConnectionHandle::Make(15, 255, ConnectionHandle::kMaxSlots - 1, UINT32_MAX);
    CHECK(h.group() == 15 && h.loop() == 255 && h.slot() == ConnectionHandle::kMaxSlots - 1 &&
          h.generation() == UINT32_MAX);
    CHECK(h.raw() == UINT64_MAX);
    CHECK(ConnectionHandle(h.raw()) == h);

    // 各字段互不串位
    const auto a = ConnectionHandle::Make(1, 2, 3, 4);
    CHECK(a.group() == 1 && a.loop() == 2 && a.slot() == 3 && a.generation() == 4);
    CHECK(a.ToString() == "g1.l2#3.4");
    CHECK(a != ConnectionHandle::Make(1, 2, 3, 5));
    CHECK(a != ConnectionHandle::Make(1, 2, 4, 4));
    CHECK(a != ConnectionHandle::Make(1, 3, 3, 4));
    CHECK(a != ConnectionHandle::Make(2, 2, 3, 4));

    // raw == 0 恒为无效：generation 从 1 起，所以真实的句柄不可能是 0
    CHECK(!ConnectionHandle{}.valid() && ConnectionHandle{}.raw() == 0);
    CHECK(!ConnectionHandle::Make(0, 0, 0, 0).valid());
    CHECK(ConnectionHandle::Make(0, 0, 0, 1).valid());

    std::unordered_set<ConnectionHandle> set;
    set.insert(a);
    set.insert(a);
    set.insert(ConnectionHandle::Make(1, 2, 3, 5));
    CHECK(set.size() == 2);
}

// 主线程与多个线程经句柄发送：到达、各线程内保序；Close 之后连接以 LocalClose 恰关一次。
void TestCrossThreadSendAndClose()
{
    const LeakCheck leak;
    Recorder rec;
    GroupServer s(rec);
    TestClient c(s.ep());
    const ConnectionHandle h = rec.WaitHandle(0);
    CHECK(h.valid() && h.group() == GroupAccess::Id(s.group) && h.loop() == 0);

    CHECK(h.Send(Buf("hello")) == SendResult::Ok);
    CHECK(c.RecvFrame() == "hello");

    constexpr int kThreads = 4;
    constexpr int kPerThread = 300;
    std::vector<std::thread> ths;
    for (int t = 0; t < kThreads; ++t)
    {
        ths.emplace_back(
            [&, t]
            {
                for (int n = 0; n < kPerThread; ++n)
                    CHECK(h.Send(Buf("t" + std::to_string(t) + "-" + std::to_string(n))) == SendResult::Ok);
            });
    }
    std::vector<int> next(kThreads, 0);
    for (int i = 0; i < kThreads * kPerThread; ++i)
    {
        const std::string f = c.RecvFrame();
        const std::size_t dash = f.find('-');
        CHECK(f[0] == 't' && dash != std::string::npos);
        const int t = std::stoi(f.substr(1, dash - 1));
        CHECK(std::stoi(f.substr(dash + 1)) == next[static_cast<std::size_t>(t)]++); // 每个线程内保序
    }
    for (auto& th : ths)
        th.join();

    h.Close();
    CHECK(c.WaitClosed());
    const auto closed = rec.WaitClosed(1);
    CHECK(closed[0].first == h && closed[0].second == CloseReason::LocalClose);
    CHECK(WaitUntil([&] { return s.Drained(); }));
    CHECK(h.Send(Buf("late")) == SendResult::Closed);
    h.Close(); // 对已过期的句柄是空操作
    std::this_thread::sleep_for(50ms);
    CHECK(rec.ClosedCount() == 1);
}

// 槽被复用之后，旧句柄既不能发到新连接、也关不掉新连接。
// 容量 2、FIFO 复用：A 占 0 → B 占 1 → C 占 0（generation 变 2）。
void TestStaleHandleDoesNotReachReusedSlot()
{
    const LeakCheck leak;
    Recorder rec;
    GroupServer s(rec, {1, 2});

    auto a = std::make_unique<TestClient>(s.ep());
    const ConnectionHandle ha = rec.WaitHandle(0);
    a.reset();
    CHECK(WaitUntil([&] { return s.Drained(); }));
    auto b = std::make_unique<TestClient>(s.ep());
    rec.WaitHandle(1);
    b.reset();
    CHECK(WaitUntil([&] { return s.Drained(); }));
    TestClient c(s.ep());
    const ConnectionHandle hc = rec.WaitHandle(2);
    CHECK(hc.slot() == ha.slot() && hc.generation() != ha.generation());

    // 发送方的第一道校验：generation 不符 → Closed
    CHECK(ha.Send(Buf("to-A")) == SendResult::Closed);
    ha.Close(); // 空操作，关不掉 C

    // 第二道校验：绕过发送方检查，直接把旧句柄的任务塞进 loop 队列——loop 线程执行时发现已换代，必须丢弃
    s.loop().PostOp(EventLoop::Op::Send, ha, 0, Buf("stale-send"));
    s.loop().PostOp(EventLoop::Op::Close, ha, static_cast<std::uint32_t>(CloseReason::LocalClose), {});

    CHECK(hc.Send(Buf("to-C")) == SendResult::Ok);
    CHECK(c.RecvFrame() == "to-C"); // 第一帧就是它：旧句柄的发送没有串进来，C 也还活着
    // 记账在 sendmsg 返回之后才扣：客户端已经收到时 IO 线程可能还没扣，要等。
    CHECK(WaitUntil([&] { return s.loop().table().PendingBytes(hc.slot()) == 0; }));
    CHECK(rec.ClosedCount() == 2); // A、B；C 没被关
}

void TestInvalidHandlesAreClosed()
{
    const LeakCheck leak;
    Recorder rec;
    GroupServer s(rec, {1, 8});
    TestClient c(s.ep());
    const ConnectionHandle h = rec.WaitHandle(0);
    const std::uint32_t g = h.group();

    const ConnectionHandle bad[] = {
        ConnectionHandle{},                                          // raw == 0
        ConnectionHandle::Make(g, 0, h.slot(), h.generation() + 1),  // generation 不符
        ConnectionHandle::Make(g, 200, h.slot(), h.generation()),    // loop 越界
        ConnectionHandle::Make(g, 0, 7, 1),                          // 槽存在但没有连接
        ConnectionHandle::Make(g, 0, 100000, 1),                     // slot 越界
        ConnectionHandle::Make((g + 1) % 16, 0, h.slot(), h.generation()), // group 未注册
    };
    for (const ConnectionHandle b : bad)
    {
        CHECK(b.Send(Buf("x")) == SendResult::Closed);
        b.Close(); // 不崩、不影响别的连接
    }
    CHECK(h.Send(Buf("still-alive")) == SendResult::Ok);
    CHECK(c.RecvFrame() == "still-alive");
    CHECK(rec.ClosedCount() == 0);
}

// group 停掉之后它的句柄解析不到：Send 返回 Closed、Close 是空操作，没有悬空访问。
void TestHandleOfStoppedGroup()
{
    const LeakCheck leak;
    Recorder rec;
    ConnectionHandle h;
    {
        GroupServer s(rec);
        TestClient c(s.ep());
        h = rec.WaitHandle(0);
        CHECK(h.Send(Buf("a")) == SendResult::Ok);
        CHECK(c.RecvFrame() == "a");
        s.Stop();
        CHECK(h.Send(Buf("b")) == SendResult::Closed);
        h.Close();
    }
    CHECK(h.Send(Buf("c")) == SendResult::Closed); // group 已销毁
}

// 慢消费者（跨线程路径）：客户端不读，积压超过水位 → Send 返回 Closed，连接以 SlowConsumer 恰关一次。
void TestSlowConsumerViaHandle()
{
    const LeakCheck leak;
    Recorder rec;
    GroupServer s(rec, {1, 64}, ConnectionOptions{512 * 1024});
    TestClient c(s.ep()); // 不读
    const ConnectionHandle h = rec.WaitHandle(0);

    bool closed = false;
    for (int i = 0; i < 400 && !closed; ++i) // 内核缓冲吸收几 MB 之后积压才会涨；最多发 100MB
        closed = h.Send(SharedBuffer::Create(256 * 1024)) == SendResult::Closed;
    CHECK(closed);

    const auto done = rec.WaitClosed(1);
    CHECK(done[0].first == h && done[0].second == CloseReason::SlowConsumer);
    CHECK(c.WaitClosed());
    CHECK(WaitUntil([&] { return s.Drained(); }));
    CHECK(s.loop().table().PendingBytes(h.slot()) == 0); // 回收时清零
    std::this_thread::sleep_for(50ms);
    CHECK(rec.ClosedCount() == 1);
}

// pending_bytes 的记账：发送积压时涨起来，对端读完之后回到 0（既不漏减也不多减）。
void TestPendingBytesDrainsToZero()
{
    const LeakCheck leak;
    Recorder rec;
    GroupServer s(rec, {1, 64}, ConnectionOptions{256ull << 20});
    TestClient c(s.ep());
    const ConnectionHandle h = rec.WaitHandle(0);

    constexpr int kFrames = 16;
    constexpr std::size_t kSize = 2 * 1024 * 1024; // 共 32MB，远超内核缓冲
    for (int i = 0; i < kFrames; ++i)
    {
        SharedBuffer b = SharedBuffer::Create(kSize);
        std::memset(b.MutableBytes().data(), 'a' + i, kSize);
        CHECK(h.Send(std::move(b)) == SendResult::Ok);
    }
    CHECK(WaitUntil([&] { return s.loop().table().PendingBytes(h.slot()) > 0; })); // 客户端没读，必有积压

    for (int i = 0; i < kFrames; ++i)
    {
        const std::string f = c.RecvFrame();
        CHECK(f.size() == kSize && f.front() == 'a' + i && f.back() == 'a' + i);
    }
    CHECK(WaitUntil([&] { return s.loop().table().PendingBytes(h.slot()) == 0; }));
    CHECK(rec.ClosedCount() == 0); // 水位没有触及，连接仍然健在
}

// 多个线程不停 Send，同时另一个线程 Close：不崩、不死锁，OnClosed 恰一次（tsan 下覆盖校验与回收的竞争）。
void TestSendRacingClose()
{
    const LeakCheck leak;
    Recorder rec;
    GroupServer s(rec, {1, 64}, ConnectionOptions{1ull << 30});
    TestClient c(s.ep());
    const ConnectionHandle h = rec.WaitHandle(0);

    // 每个线程发送有上限：任务队列无界，Close 任务排在队尾，且 moodycamel 无 token 出队时先排空较新的生产者，
    // 主线程这个最老的生产者要等发送线程的积压清完才轮得到。不设上限时 4 个线程比 IO 线程快得多，
    // 队列越灌越长、Close 永远等不到（实测内存被吃光）。
    constexpr int kThreads = 4;
    constexpr int kMaxPerThread = 20000;
    std::vector<std::thread> ths;
    for (int t = 0; t < kThreads; ++t)
    {
        ths.emplace_back(
            [&]
            {
                for (int n = 0; n < kMaxPerThread && h.Send(Buf("x")) == SendResult::Ok; ++n)
                {
                }
            });
    }
    std::this_thread::sleep_for(5ms);
    h.Close();
    for (auto& th : ths)
        th.join(); // Close 生效之后 Send 返回 Closed，线程提前退出；否则发满上限也退出
    CHECK(c.WaitClosed());
    const auto done = rec.WaitClosed(1);
    CHECK(done[0].second == CloseReason::LocalClose);
    CHECK(WaitUntil([&] { return s.Drained(); }));
    std::this_thread::sleep_for(50ms);
    CHECK(rec.ClosedCount() == 1);
}


// ---- 批内合并（EventLoop::RunSendRun / IConnection::SendBatchFromTask）----
//
// 一批任务里相邻的、发往同一条连接的 Send 会合成一次 sendmsg。要让这一段确定地凑在同一批里，先堵住 IO 线程：
// 此后经句柄的 Send / Close 都排在队列里，放开时被同一批取走（一批最多 64 个任务，一段最多 16 条）。

// 读到 EOF 为止，返回 EOF 之前还收到了多少字节（0 = 没有多余的数据）。
std::size_t DrainUntilEof(TestClient& c)
{
    std::size_t total = 0;
    char buf[4096];
    for (int spins = 0; spins < 100; ++spins)
    {
        if (!c.Wait(POLLIN, 200ms))
            continue;
        const ssize_t n = ::recv(c.fd(), buf, sizeof(buf), 0);
        if (n == 0)
            return total;
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            return total; // ECONNRESET
        if (n > 0)
            total += static_cast<std::size_t>(n);
    }
    CHECK(false && "DrainUntilEof：对端没有关闭");
    return total;
}

// 各种长度的同连接连续 Send（跨过一段 16 条与一批 64 个任务的边界）：帧完整、顺序不变，计数与字节数对得上。
void TestBatchedSendsKeepOrderAndAccounting()
{
    const LeakCheck leak;
    Recorder rec;
    GroupServer s(rec);
    TestClient c(s.ep());
    const ConnectionHandle h = rec.WaitHandle(0);

    std::uint64_t frames = 0;
    std::uint64_t bytes = 0;
    for (const int round_n : {1, 2, 3, 15, 16, 17, 31, 32, 33, 40, 64, 65, 100, 200})
    {
        LoopGate gate(s.loop());
        std::vector<std::string> sent;
        for (int i = 0; i < round_n; ++i)
        {
            sent.push_back("r" + std::to_string(round_n) + "-" + std::string(static_cast<std::size_t>(i % 7), 'x') +
                           std::to_string(i));
            CHECK(h.Send(Buf(sent.back())) == SendResult::Ok);
            bytes += 4 + sent.back().size();
        }
        frames += static_cast<std::uint64_t>(round_n);
        gate.Open();
        for (const auto& want : sent)
            CHECK(c.RecvFrame() == want);
    }
    CHECK(WaitUntil([&] { return s.loop().table().PendingBytes(h.slot()) == 0; })); // 记账不漏减、不多减
    CHECK(WaitUntil(
        [&]
        {
            const NetStats st = s.group.GetStats();
            return st.messages_out == frames && st.bytes_out == bytes;
        }));
    CHECK(rec.ClosedCount() == 0);
}

// 两条连接交错：A A B A / A A B A ……（段长 2、1 交替）。每条连接内各自保序，互不串。
void TestBatchedSendsWithInterleavedConnections()
{
    const LeakCheck leak;
    Recorder rec;
    GroupServer s(rec);
    TestClient c1(s.ep());
    const ConnectionHandle h1 = rec.WaitHandle(0);
    TestClient c2(s.ep());
    const ConnectionHandle h2 = rec.WaitHandle(1);

    constexpr int kRounds = 60;
    {
        LoopGate gate(s.loop());
        for (int i = 0; i < kRounds; ++i)
        {
            const std::string n = std::to_string(i);
            CHECK(h1.Send(Buf("A" + n + ".0")) == SendResult::Ok);
            CHECK(h1.Send(Buf("A" + n + ".1")) == SendResult::Ok);
            CHECK(h2.Send(Buf("B" + n)) == SendResult::Ok);
            CHECK(h1.Send(Buf("A" + n + ".2")) == SendResult::Ok);
        }
    }
    for (int i = 0; i < kRounds; ++i)
    {
        const std::string n = std::to_string(i);
        CHECK(c1.RecvFrame() == "A" + n + ".0");
        CHECK(c1.RecvFrame() == "A" + n + ".1");
        CHECK(c1.RecvFrame() == "A" + n + ".2");
        CHECK(c2.RecvFrame() == "B" + n);
    }
    CHECK(WaitUntil(
        [&]
        { return s.loop().table().PendingBytes(h1.slot()) == 0 && s.loop().table().PendingBytes(h2.slot()) == 0; }));
}

// 一段 Send 之后紧跟 Close / CloseGracefully：段先执行完，再关——与逐条执行的顺序一致；关闭之后再来的 Send 一条也发不出去。
void TestBatchedSendsThenCloseKeepOrder()
{
    const LeakCheck leak;
    Recorder rec;
    GroupServer s(rec);
    {
        TestClient c(s.ep());
        const ConnectionHandle h = rec.WaitHandle(0);
        {
            LoopGate gate(s.loop());
            for (int i = 0; i < 20; ++i)
                CHECK(h.Send(Buf("g" + std::to_string(i))) == SendResult::Ok);
            h.CloseGracefully(2000ms);
            for (int i = 0; i < 3; ++i)
                CHECK(h.Send(Buf("late")) == SendResult::Ok); // 句柄这时还有效，投递成功；执行时连接已在优雅关闭，被丢弃
        }
        for (int i = 0; i < 20; ++i)
            CHECK(c.RecvFrame() == "g" + std::to_string(i));
        CHECK(DrainUntilEof(c) == 0); // 没有 "late"
        CHECK(rec.WaitClosed(1)[0].second == CloseReason::LocalClose);
    }
    {
        TestClient c(s.ep());
        const ConnectionHandle h = rec.WaitHandle(1);
        {
            LoopGate gate(s.loop());
            for (int i = 0; i < 5; ++i)
                CHECK(h.Send(Buf("h" + std::to_string(i))) == SendResult::Ok);
            h.Close(); // 立即关：Close 之前的 Send 已经写进内核，照常送达
        }
        for (int i = 0; i < 5; ++i)
            CHECK(c.RecvFrame() == "h" + std::to_string(i));
        CHECK(DrainUntilEof(c) == 0);
    }
    CHECK(WaitUntil([&] { return s.Drained(); }));
}

// 写不下：一段 16 帧 × 1MB，内核缓冲装不下，某一帧可能只写出一部分——剩下的按序入队、EPOLLOUT 排空，内容逐字节对得上。
void TestBatchedPartialWriteQueuesTheRestInOrder()
{
    const LeakCheck leak;
    Recorder rec;
    GroupServer s(rec, {1, 64}, ConnectionOptions{512ull << 20});
    TestClient c(s.ep());
    const ConnectionHandle h = rec.WaitHandle(0);

    constexpr int kFrames = 16;
    constexpr std::size_t kSize = 1024 * 1024;
    {
        LoopGate gate(s.loop());
        for (int i = 0; i < kFrames; ++i)
        {
            SharedBuffer b = SharedBuffer::Create(kSize);
            std::memset(b.MutableBytes().data(), 'a' + i, kSize);
            // 首尾字节各不相同，才能发现"偏移错位"（整帧同一个字节看不出来）
            b.MutableBytes()[0] = static_cast<std::byte>(i);
            b.MutableBytes()[kSize - 1] = static_cast<std::byte>(100 + i);
            CHECK(h.Send(std::move(b)) == SendResult::Ok);
        }
    }
    CHECK(WaitUntil([&] { return s.loop().table().PendingBytes(h.slot()) > 0; })); // 客户端没读，必有积压
    for (int i = 0; i < kFrames; ++i)
    {
        const std::string f = c.RecvFrame();
        CHECK(f.size() == kSize);
        CHECK(static_cast<unsigned char>(f.front()) == i && static_cast<unsigned char>(f.back()) == 100 + i);
        CHECK(std::all_of(f.begin() + 1, f.end() - 1, [&](char ch) { return ch == static_cast<char>('a' + i); }));
    }
    CHECK(WaitUntil([&] { return s.loop().table().PendingBytes(h.slot()) == 0; }));
    CHECK(WaitUntil([&] { return s.group.GetStats().messages_out == kFrames; }));
    CHECK(rec.ClosedCount() == 0);
}

// 段里夹了一个超过 codec 上限的 payload：退回逐条，行为与没合并时一样——它之前的帧送达，它触发协议错误关连接，之后的被丢弃。
void TestBatchWithOversizePayloadFallsBackToOneByOne()
{
    const LeakCheck leak;
    Recorder rec;
    GroupServer s(rec, {1, 64}, ConnectionOptions{64ull << 20});
    TestClient c(s.ep());
    const ConnectionHandle h = rec.WaitHandle(0);
    {
        LoopGate gate(s.loop());
        CHECK(h.Send(Buf("s0")) == SendResult::Ok);
        CHECK(h.Send(Buf("s1")) == SendResult::Ok);
        SharedBuffer big = SharedBuffer::Create(5 * 1024 * 1024); // GroupServer 的 codec 上限是 4MB
        std::memset(big.MutableBytes().data(), 'z', big.size());
        CHECK(h.Send(std::move(big)) == SendResult::Ok);
        CHECK(h.Send(Buf("s3")) == SendResult::Ok);
    }
    CHECK(c.RecvFrame() == "s0");
    CHECK(c.RecvFrame() == "s1");
    CHECK(DrainUntilEof(c) == 0); // 没有大帧、也没有 s3
    CHECK(rec.WaitClosed(1)[0].second == CloseReason::ProtocolError);
    CHECK(WaitUntil([&] { return s.Drained(); }));
}

// 迟到的段：连接在这一段执行之前已经被关掉（先排在队里的 Post 关它），整段丢弃，不崩、不发。
void TestBatchedSendsToConnectionClosedBeforeExecution()
{
    const LeakCheck leak;
    Recorder rec;
    GroupServer s(rec);
    TestClient c(s.ep());
    const ConnectionHandle h = rec.WaitHandle(0);
    {
        LoopGate gate(s.loop());
        s.loop().Post(
            [&]
            {
                if (IConnection* conn = s.loop().table().Current(h.slot(), h.generation()))
                    conn->Close();
            });
        for (int i = 0; i < 8; ++i)
            CHECK(h.Send(Buf("never" + std::to_string(i))) == SendResult::Ok); // 句柄此刻有效，执行时连接已关
    }
    CHECK(DrainUntilEof(c) == 0);
    CHECK(rec.WaitClosed(1)[0].second == CloseReason::LocalClose);
    CHECK(WaitUntil([&] { return s.Drained(); }));
    CHECK(s.group.GetStats().messages_out == 0);
}

} // namespace

int main()
{
    TestHandlePacking();
    TestCrossThreadSendAndClose();
    TestStaleHandleDoesNotReachReusedSlot();
    TestInvalidHandlesAreClosed();
    TestHandleOfStoppedGroup();
    TestSlowConsumerViaHandle();
    TestPendingBytesDrainsToZero();
    TestSendRacingClose();
    TestBatchedSendsKeepOrderAndAccounting();
    TestBatchedSendsWithInterleavedConnections();
    TestBatchedSendsThenCloseKeepOrder();
    TestBatchedPartialWriteQueuesTheRestInOrder();
    TestBatchWithOversizePayloadFallsBackToOneByOne();
    TestBatchedSendsToConnectionClosedBeforeExecution();
    std::puts("connection_handle_test: ok");
    return 0;
}
