// 握手阶段（OnConnected 延后到所有握手来源完成）：用一个假的"一字节握手" codec 驱动明文连接，不依赖任何 TLS / WebSocket。
// 覆盖：服务端握手完成前业务看不见连接、握手应答与随后 Send 的保序、握手超时、对端中途断开、codec 拒绝（CloseAfterFlush）、
// HandshakeDone 幂等、CloseAfterFlush 带出原因与 code、OnClosing 与 CloseGracefully；
// 客户端握手后收发、握手必败时退避会增长并持续重试（不能因为 TCP 已连上就重置退避）、握手中途 Stop 不挂住析构。
// 无测试框架依赖：失败即 abort。

#include <poll.h>
#include <sys/socket.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "base/net/codec/codec_host_interface.h"
#include "base/net/codec/length_prefix_codec.h"
#include "base/net/loop/event_loop_group.h"
#include "base/net/stream/stream_client.h"
#include "base/net/stream/stream_server.h"
#include "base/net/testing/hello_codec_test_util.h"
#include "base/net/testing/net_test_util.h"

using namespace breeze;
using namespace breeze::testing;

namespace
{

std::span<const std::byte> B(std::string_view s) { return RawBytes(s); }

using Mode = HelloMode;

// ---- 夹具 ----

StreamServerOptions ServerOptions(std::chrono::milliseconds handshake_timeout)
{
    StreamServerOptions o;
    o.connection.handshake_timeout = handshake_timeout;
    return o;
}

struct Server
{
    explicit Server(Mode mode, std::chrono::milliseconds handshake_timeout = 5s)
        : factory(true, mode)
        , group({1, 64})
        , server(group, *Endpoint::Tcp("127.0.0.1", 0), ServerOptions(handshake_timeout), rec, factory)
    {
        rec.on_message = Echo;
        CHECK(group.Start() == 0);
        CHECK(server.Start() == 0);
    }
    NetStats stats() const { return group.GetStats(); }
    std::uint64_t Failed(HandshakeFailure f) const { return stats().handshake_failed[static_cast<std::size_t>(f)]; }

    EventRecorder rec;
    HelloCodecFactory factory;
    EventLoopGroup group;
    StreamServer server;
};

struct ClientSide
{
    ClientSide()
        : factory(false, Mode::Normal)
        , group({1, 64})
    {
        CHECK(group.Start() == 0);
    }
    EventRecorder rec;
    HelloCodecFactory factory;
    EventLoopGroup group;
};

// 直接读 n 个原始字节（不分帧）。
std::string ReadRaw(TestClient& c, std::size_t n)
{
    std::string out;
    while (out.size() < n)
    {
        CHECK(c.Wait(POLLIN, kWait));
        char buf[64];
        const ssize_t r = ::recv(c.fd(), buf, std::min(sizeof(buf), n - out.size()), 0);
        CHECK(r > 0);
        out.append(buf, static_cast<std::size_t>(r));
    }
    return out;
}

ReconnectPolicy FastPolicy() { return {20ms, 100ms, 2.0, 0.0}; }

// ---- 服务端 ----

// 握手没完成，业务看不见这条连接；握手应答（SendRaw）先于 OnConnected 里发的数据（Send），两者保序。
void TestServerDefersOnConnectedUntilHandshakeDone()
{
    const LeakCheck leak;
    Server s(Mode::Normal);
    s.rec.on_connected = [](IConnection& c) { (void)c.Send(B("hi")); };
    {
        TestClient c(s.server.endpoint());
        CHECK(WaitUntil([&] { return s.stats().handshaking_connections == 1; }));
        std::this_thread::sleep_for(100ms);
        CHECK(s.rec.connected_count() == 0); // 握手还没完成

        c.SendAll(B("H"));
        CHECK(ReadRaw(c, 1) == "W");
        CHECK(c.RecvFrame() == "hi"); // 在 'W' 之后
        s.rec.WaitConnected(0);
        c.SendFrame("echo-me");
        CHECK(c.RecvFrame() == "echo-me");
        CHECK(s.stats().handshaking_connections == 0);
    }
    s.rec.WaitClosed(0);
    s.rec.CheckInvariants();
    CHECK(s.Failed(HandshakeFailure::Io) == 0 && s.Failed(HandshakeFailure::Timeout) == 0);
}

// 握手超时：服务端按 handshake_timeout 关闭；没回调过 OnConnected，所以也没有 OnClosed，只记统计。
void TestHandshakeTimeoutClosesSilently()
{
    const LeakCheck leak;
    Server s(Mode::Normal, 200ms);
    {
        TestClient c(s.server.endpoint());
        CHECK(c.WaitClosed(5s));
        CHECK(WaitUntil([&] { return s.Failed(HandshakeFailure::Timeout) == 1; }));
        CHECK(s.rec.connected_count() == 0 && s.rec.closed_count() == 0);
        CHECK(s.stats().handshaking_connections == 0);
    }
}

// 对端在握手中途断开：没有回调，记 Io。
void TestPeerDisconnectDuringHandshake()
{
    const LeakCheck leak;
    Server s(Mode::Normal);
    {
        TestClient c(s.server.endpoint());
        CHECK(WaitUntil([&] { return s.stats().handshaking_connections == 1; }));
        c.Close();
        CHECK(WaitUntil([&] { return s.Failed(HandshakeFailure::Io) == 1; }));
        CHECK(WaitUntil([&] { return s.stats().handshaking_connections == 0; }));
        CHECK(s.rec.connected_count() == 0 && s.rec.closed_count() == 0);
    }
}

// codec 在握手中途拒绝：写完错误应答再关（CloseAfterFlush），原因是 ProtocolError → 记 Upgrade；仍然没有任何回调。
void TestCodecRejectsDuringHandshake()
{
    const LeakCheck leak;
    Server s(Mode::Reject);
    {
        TestClient c(s.server.endpoint());
        c.SendAll(B("X"));
        CHECK(ReadRaw(c, 1) == "E");
        CHECK(c.WaitClosed(5s)); // 写完后服务端 shutdown(SHUT_WR)，客户端读到 EOF
        c.Close();
        CHECK(WaitUntil([&] { return s.Failed(HandshakeFailure::Upgrade) == 1; }));
        CHECK(s.rec.connected_count() == 0 && s.rec.closed_count() == 0);
        CHECK(s.stats().handshaking_connections == 0);
    }
}

// HandshakeDone 重复报告无害：OnConnected 恰好一次。
void TestHandshakeDoneIsIdempotent()
{
    const LeakCheck leak;
    Server s(Mode::DoneTwice);
    {
        TestClient c(s.server.endpoint());
        c.SendAll(B("H"));
        CHECK(ReadRaw(c, 1) == "W");
        s.rec.WaitConnected(0);
        c.SendFrame("x");
        CHECK(c.RecvFrame() == "x");
        CHECK(s.rec.connected_count() == 1);
    }
    s.rec.WaitClosed(0);
    s.rec.CheckInvariants();
}

// CloseAfterFlush 的原因与 code 在对端关闭之后仍然带出：OnClosed 报 PeerClosed 与 4001，不是 LocalClose。
void TestCloseAfterFlushKeepsReasonAndCode()
{
    const LeakCheck leak;
    Server s(Mode::Normal);
    {
        TestClient c(s.server.endpoint());
        c.SendAll(B("H"));
        CHECK(ReadRaw(c, 1) == "W");
        s.rec.WaitConnected(0);
        c.SendFrame("bye");
        CHECK(ReadRaw(c, 1) == "B");
        CHECK(c.WaitClosed(5s)); // 写完后服务端 shutdown(SHUT_WR)
        c.Close();               // 我们这一侧关闭：服务端读到 EOF，收尾时换回记下的原因
    }
    const auto closed = s.rec.WaitClosed(0);
    CHECK(closed.reason == CloseReason::PeerClosed && closed.err == 4001);
    CHECK(s.stats().closed[static_cast<std::size_t>(CloseReason::PeerClosed)] == 1);
    s.rec.CheckInvariants();
}

// CloseGracefully 会先调 codec 的 OnClosing（它在这里发了一个原始字节），再 shutdown。
void TestOnClosingRunsOnCloseGracefully()
{
    const LeakCheck leak;
    Server s(Mode::Normal);
    s.rec.on_connected = [](IConnection& c) { c.CloseGracefully(2s); };
    {
        TestClient c(s.server.endpoint());
        c.SendAll(B("H"));
        CHECK(ReadRaw(c, 2) == "WB"); // 握手应答，然后是 OnClosing 的原始字节
        CHECK(c.WaitClosed(5s));
    }
    const auto closed = s.rec.WaitClosed(0);
    CHECK(closed.reason == CloseReason::LocalClose);
    s.rec.CheckInvariants();
}

// ---- 客户端 ----

void TestClientHandshakeThenEcho()
{
    const LeakCheck leak;
    Server s(Mode::Normal);
    ClientSide cs;
    {
        StreamClient c(cs.group, s.server.endpoint(), {}, FastPolicy(), cs.rec, cs.factory);
        CHECK(c.Start() == 0);
        cs.rec.WaitConnected(0);
        CHECK(c.handle().valid()); // 句柄在握手完成、OnConnected 之前发布
        CHECK(c.Send(Buf("ping")) == SendResult::Ok);
        CHECK(cs.rec.WaitMessages(1));
        CHECK(cs.rec.payloads()[0] == "ping");
        CHECK(cs.group.GetStats().connected == 1);
        CHECK(cs.group.GetStats().handshaking_connections == 0);
    }
    cs.rec.CheckInvariants();
}

// 握手必败（服务端吞掉握手字节、永不应答）：客户端按 handshake_timeout 判失败，退避**会增长**并持续重试。
// 若 TCP 一连上就重置退避，间隔会一直是初始值——这条用例守的就是这个。没有 OnConnected / OnClosed。
void TestClientHandshakeFailureBacksOffAndRetries()
{
    const LeakCheck leak;
    Server s(Mode::NeverDone);
    ClientSide cs;

    std::mutex mu;
    std::vector<std::chrono::steady_clock::time_point> times;
    std::vector<std::pair<int, bool>> fails;
    StreamClientOptions co;
    co.connection.handshake_timeout = 100ms;
    co.on_connect_failed = [&](int e, bool retry) noexcept
    {
        const std::lock_guard lk(mu);
        times.push_back(std::chrono::steady_clock::now());
        fails.emplace_back(e, retry);
    };
    {
        StreamClient c(cs.group, s.server.endpoint(), std::move(co), ReconnectPolicy{40ms, 400ms, 2.0, 0.0}, cs.rec, cs.factory);
        CHECK(c.Start() == 0);
        CHECK(WaitUntil(
            [&]
            {
                const std::lock_guard lk(mu);
                return fails.size() >= 5;
            },
            15s));
    }
    const std::lock_guard lk(mu);
    for (const auto& [e, retry] : fails)
        CHECK(e == ETIMEDOUT && retry); // 映射成固定 errno，可重试
    // 相邻两次失败的间隔 ≈ handshake_timeout(100ms) + 退避(40, 80, 160, 320ms)：必须明显变长。
    const auto gap = [&](std::size_t i) { return std::chrono::duration_cast<std::chrono::milliseconds>(times[i + 1] - times[i]); };
    CHECK(gap(3) >= gap(0) + 200ms);
    CHECK(cs.rec.connected_count() == 0 && cs.rec.closed_count() == 0);
    CHECK(cs.group.GetStats().handshake_failed[static_cast<std::size_t>(HandshakeFailure::Timeout)] >= 5);
}

// 握手中途 Stop：半开连接被关掉，不重连、没有任何回调，客户端析构不会等到超时。
void TestStopDuringHandshake()
{
    const LeakCheck leak;
    Server s(Mode::NeverDone);
    ClientSide cs;

    std::atomic<int> failures{0};
    StreamClientOptions co;
    co.connection.handshake_timeout = 30s; // 足够长：确保 Stop 时还在握手
    co.on_connect_failed = [&](int, bool) noexcept { failures.fetch_add(1); };
    const auto t0 = std::chrono::steady_clock::now();
    {
        StreamClient c(cs.group, s.server.endpoint(), std::move(co), FastPolicy(), cs.rec, cs.factory);
        CHECK(c.Start() == 0);
        CHECK(WaitUntil([&] { return cs.group.GetStats().handshaking_connections == 1; }));
        c.Stop();
        CHECK(WaitUntil([&] { return cs.group.GetStats().handshaking_connections == 0; }));
        std::this_thread::sleep_for(300ms); // 不重连
        CHECK(cs.group.GetStats().handshaking_connections == 0);
        CHECK(!c.handle().valid());
    }
    CHECK(std::chrono::steady_clock::now() - t0 < 3s); // 析构没有卡在 awaiting_closed 上
    CHECK(failures.load() == 0);                       // 主动 Stop 不是连接失败
    CHECK(cs.rec.connected_count() == 0 && cs.rec.closed_count() == 0);
    for (const auto n : cs.group.GetStats().handshake_failed)
        CHECK(n == 0); // 主动关闭不计入握手失败
}

} // namespace

int main()
{
    TestServerDefersOnConnectedUntilHandshakeDone();
    TestHandshakeTimeoutClosesSilently();
    TestPeerDisconnectDuringHandshake();
    TestCodecRejectsDuringHandshake();
    TestHandshakeDoneIsIdempotent();
    TestCloseAfterFlushKeepsReasonAndCode();
    TestOnClosingRunsOnCloseGracefully();
    TestClientHandshakeThenEcho();
    TestClientHandshakeFailureBacksOffAndRetries();
    TestStopDuringHandshake();
    std::puts("handshake_test: ok");
    return 0;
}
