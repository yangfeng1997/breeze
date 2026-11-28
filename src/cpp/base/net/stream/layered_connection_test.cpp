// 分层连接（LayeredStreamConnection）：用假的字节流层（握手 "HELO" / "OK!!"，之后逐字节异或）驱动，不依赖 BoringSSL。
// 覆盖：经层的回显（线上确实是被层改过的字节）、握手字节被任意切分、握手完成的同一次读里就带着业务帧（OnConnected 仍先于
// 第一条消息）、大 payload 超过写预算与大量积压帧保序、层出错（握手前 / 握手后）、握手超时与中途断开、优雅关闭时调层的
// Shutdown、跨线程句柄发送、PauseRead / ResumeRead、客户端侧（含握手必败时退避增长）、层与 codec 的双重握手（wss 的形状）、
// "没有层但 codec 要改写出站 payload"（WebSocket 客户端的形状：走恒等层，payload 的拷贝被改写、原件不动）、UDS 上配层被拒。
// 无测试框架依赖：失败即 abort。

#include <poll.h>
#include <sys/socket.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
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
#include "base/net/testing/fake_layer_test_util.h"
#include "base/net/testing/hello_codec_test_util.h"
#include "base/net/testing/net_test_util.h"

using namespace breeze;
using namespace breeze::testing;

namespace
{

constexpr std::size_t kMaxFrame = 16u << 20;

std::span<const std::byte> B(std::string_view s) { return RawBytes(s); }

// ---- 夹具 ----

struct LServer
{
    explicit LServer(FakeLayerMode mode = FakeLayerMode::Normal,
                     std::size_t fail_after = 0,
                     std::unique_ptr<IFrameCodecFactory> codec_factory = nullptr,
                     std::chrono::milliseconds handshake_timeout = 5s)
        : layer(std::make_shared<FakeLayerFactory>(mode, fail_after))
        , codec(codec_factory ? std::move(codec_factory) : std::make_unique<LengthPrefixCodecFactory>(kMaxFrame))
        , group({1, 64})
        , server(group, *Endpoint::Tcp("127.0.0.1", 0), Options(layer, handshake_timeout), rec, *codec)
    {
        rec.on_message = Echo;
        CHECK(group.Start() == 0);
        CHECK(server.Start() == 0);
    }
    static StreamServerOptions Options(std::shared_ptr<FakeLayerFactory> l, std::chrono::milliseconds handshake_timeout)
    {
        StreamServerOptions o;
        o.tls = std::move(l);
        o.connection.handshake_timeout = handshake_timeout;
        o.connection.high_water_mark = 64u << 20; // 大 payload 的用例要排得下
        return o;
    }
    NetStats stats() const { return group.GetStats(); }
    std::uint64_t HsFailed(HandshakeFailure f) const { return stats().handshake_failed[static_cast<std::size_t>(f)]; }

    EventRecorder rec;
    std::shared_ptr<FakeLayerFactory> layer;
    std::unique_ptr<IFrameCodecFactory> codec;
    EventLoopGroup group;
    StreamServer server;
};

struct LClient
{
    explicit LClient(std::unique_ptr<IFrameCodecFactory> codec_factory = nullptr)
        : layer(std::make_shared<FakeLayerFactory>())
        , codec(codec_factory ? std::move(codec_factory) : std::make_unique<LengthPrefixCodecFactory>(kMaxFrame))
        , group({1, 64})
    {
        CHECK(group.Start() == 0);
    }
    StreamClientOptions Options() const
    {
        StreamClientOptions o;
        o.tls = layer;
        return o;
    }
    EventRecorder rec;
    std::shared_ptr<FakeLayerFactory> layer;
    std::unique_ptr<IFrameCodecFactory> codec;
    EventLoopGroup group;
};

ReconnectPolicy FastPolicy() { return {20ms, 100ms, 2.0, 0.0}; }

// ---- 对着假层说话的原始客户端 ----

std::string ReadRaw(TestClient& c, std::size_t n)
{
    static thread_local char buf[64 * 1024];
    std::string out;
    out.reserve(n);
    while (out.size() < n)
    {
        CHECK(c.Wait(POLLIN, kWait));
        const ssize_t r = ::recv(c.fd(), buf, std::min(sizeof(buf), n - out.size()), 0);
        CHECK(r > 0);
        out.append(buf, static_cast<std::size_t>(r));
    }
    return out;
}

void LayerHandshake(TestClient& c)
{
    c.SendAll(B("HELO"));
    CHECK(ReadRaw(c, 4) == "OK!!");
}

std::string FrameBytes(std::string_view payload)
{
    const auto f = TestClient::Frame(payload);
    return {reinterpret_cast<const char*>(f.data()), f.size()};
}

void SendPlainFrame(TestClient& c, std::string_view payload) { c.SendAll(B(FakeXor(FrameBytes(payload)))); }

std::string RecvPlainFrame(TestClient& c)
{
    const std::string h = FakeXor(ReadRaw(c, 4));
    const std::size_t len = (static_cast<std::size_t>(static_cast<unsigned char>(h[0])) << 24) |
                            (static_cast<std::size_t>(static_cast<unsigned char>(h[1])) << 16) |
                            (static_cast<std::size_t>(static_cast<unsigned char>(h[2])) << 8) |
                            static_cast<std::size_t>(static_cast<unsigned char>(h[3]));
    return FakeXor(ReadRaw(c, len));
}

// ---- 服务端 ----

// 回显经过层：线上读到的是被层异或过的字节；握手完成之前业务看不见这条连接。
void TestEchoThroughLayer()
{
    const LeakCheck leak;
    LServer s;
    {
        TestClient c(s.server.endpoint());
        CHECK(WaitUntil([&] { return s.stats().handshaking_connections == 1; }));
        std::this_thread::sleep_for(50ms);
        CHECK(s.rec.connected_count() == 0);
        LayerHandshake(c);
        s.rec.WaitConnected(0);
        c.SendAll(B(FakeXor(FrameBytes("hello"))));
        CHECK(ReadRaw(c, 4 + 5) == FakeXor(FrameBytes("hello"))); // 线上就是异或后的回显帧
        CHECK(s.stats().handshaking_connections == 0);
        CHECK(s.layer->counters.created == 1 && s.layer->counters.started == 1);
    }
    s.rec.WaitClosed(0);
    s.rec.CheckInvariants();
    CHECK(WaitUntil([&] { return s.layer->counters.destroyed == 1; }));
}

// 握手字节被逐字节切开：层自己攒够再判断，期间业务一直看不见连接。
void TestHandshakeBytesSplitByteByByte()
{
    const LeakCheck leak;
    LServer s;
    {
        TestClient c(s.server.endpoint());
        const std::string hello = "HELO";
        for (std::size_t i = 0; i < hello.size(); ++i)
        {
            c.SendAll(B(std::string_view(hello).substr(i, 1)));
            std::this_thread::sleep_for(30ms);
            if (i + 1 < hello.size())
                CHECK(s.rec.connected_count() == 0);
        }
        CHECK(ReadRaw(c, 4) == "OK!!");
        s.rec.WaitConnected(0);
        SendPlainFrame(c, "x");
        CHECK(RecvPlainFrame(c) == "x");
    }
    s.rec.WaitClosed(0);
    s.rec.CheckInvariants();
}

// 握手字节与一条业务帧一次发出（TLS 客户端发完 Finished 立刻发数据的形状）：同一次 read 里先完成握手、再解出这条帧。
// OnConnected 必须先于第一条消息；OnConnected 里发的数据排在回显之前。
void TestPlaintextInSameReadAsHandshake()
{
    const LeakCheck leak;
    LServer s;
    s.rec.on_connected = [](IConnection& c) { (void)c.Send(B("welcome")); };
    {
        TestClient c(s.server.endpoint());
        c.SendAll(B(std::string("HELO") + FakeXor(FrameBytes("ping"))));
        CHECK(ReadRaw(c, 4) == "OK!!");
        CHECK(RecvPlainFrame(c) == "welcome");
        CHECK(RecvPlainFrame(c) == "ping");
    }
    s.rec.WaitClosed(0);
    s.rec.CheckInvariants(); // 每条连接 C 先于 M
}

// 8MB 的 payload 远超写预算（64KB）：剩余明文入队、按 EPOLLOUT 排空，内容与顺序都不变；客户端故意晚一点才读。
void TestLargePayloadCrossesWireBudget()
{
    const LeakCheck leak;
    constexpr std::size_t kBig = 8u << 20;
    LServer s;
    s.rec.on_message = [](IConnection& c, std::string_view sv)
    {
        if (sv != "go")
            return;
        SharedBuffer big = SharedBuffer::Create(kBig);
        const auto m = big.MutableBytes();
        for (std::size_t i = 0; i < kBig; ++i)
            m[i] = static_cast<std::byte>(i * 131 + 7);
        CHECK(c.Send(big) == SendResult::Ok);
        CHECK(c.Send(Buf("tail")) == SendResult::Ok);
    };
    {
        TestClient c(s.server.endpoint());
        LayerHandshake(c);
        SendPlainFrame(c, "go");
        std::this_thread::sleep_for(300ms); // 让服务端先积压
        const std::string big = RecvPlainFrame(c);
        CHECK(big.size() == kBig);
        for (std::size_t i = 0; i < kBig; ++i)
            CHECK(static_cast<unsigned char>(big[i]) == static_cast<unsigned char>(i * 131 + 7));
        CHECK(RecvPlainFrame(c) == "tail");
    }
    s.rec.WaitClosed(0);
    s.rec.CheckInvariants();
}

// 积压里的小帧会被合并进同一个 record：3000 帧 × 3000 字节，客户端晚读，顺序与内容不变。
void TestBacklogOfManyFramesKeepsOrder()
{
    const LeakCheck leak;
    constexpr int kFrames = 3000;
    constexpr std::size_t kSize = 3000;
    LServer s;
    s.rec.on_message = [](IConnection& c, std::string_view sv)
    {
        if (sv != "go")
            return;
        for (int i = 0; i < kFrames; ++i)
        {
            char head[16];
            std::snprintf(head, sizeof(head), "idx:%05d", i);
            std::string p(kSize, static_cast<char>('a' + i % 26));
            p.replace(0, 9, head);
            CHECK(c.Send(B(p)) == SendResult::Ok);
        }
    };
    {
        TestClient c(s.server.endpoint());
        LayerHandshake(c);
        SendPlainFrame(c, "go");
        std::this_thread::sleep_for(300ms);
        for (int i = 0; i < kFrames; ++i)
        {
            const std::string p = RecvPlainFrame(c);
            CHECK(p.size() == kSize);
            char head[16];
            std::snprintf(head, sizeof(head), "idx:%05d", i);
            CHECK(p.compare(0, 9, head) == 0);
            CHECK(p[9] == static_cast<char>('a' + i % 26));
        }
    }
    s.rec.WaitClosed(0);
    s.rec.CheckInvariants();
}

// Send 返回后立即改写源内存；优雅关闭须按序排空 payload 与层在 Shutdown 时追加的字节。
void TestGracefulCloseFlushesPayloadAndLayerShutdownBytes()
{
    const LeakCheck leak;
    class GrowingLayer final : public IStreamLayer
    {
    public:
        LayerStatus Start() override { return LayerStatus::HandshakeDone; }
        LayerStatus Unwrap(std::span<std::byte> wire, PlainSink sink) override
        {
            (void)sink(wire);
            return LayerStatus::Ok;
        }
        LayerStatus Wrap(std::span<const std::byte> plain) override
        {
            wire_.Append(plain);
            return LayerStatus::Ok;
        }
        std::span<const std::byte> PendingWire() const noexcept override { return wire_.Readable(); }
        void ConsumeWire(std::size_t n) noexcept override { wire_.Consume(n); }
        LayerStatus Shutdown() override
        {
            wire_.Append(B(std::string(90000, 'b')));
            return LayerStatus::Ok;
        }
        int error_code() const noexcept override { return 0; }

    private:
        Buffer wire_;
    };
    class GrowingFactory final : public IStreamLayerFactory
    {
    public:
        std::unique_ptr<IStreamLayer> Create(LayerRole) const override { return std::make_unique<GrowingLayer>(); }
    };
    EventRecorder rec;
    LengthPrefixCodecFactory codec(kMaxFrame);
    EventLoopGroup group({1, 64});
    StreamServerOptions options;
    options.tls = std::make_shared<GrowingFactory>();
    StreamServer server(group, *Endpoint::Tcp("127.0.0.1", 0), std::move(options), rec, codec);
    rec.on_message = [](IConnection& c, std::string_view)
    {
        std::string source(20000, 'a');
        CHECK(c.Send(B(source)) == SendResult::Ok);
        std::fill(source.begin(), source.end(), 'x');
        c.CloseGracefully(5s);
    };
    CHECK(group.Start() == 0);
    CHECK(server.Start() == 0);
    {
        TestClient c(server.endpoint());
        c.SendFrame("go");
        // 不能混用 TestClient::RecvFrame 的内部预读缓冲与直接 recv，否则追加字节可能已被预读。
        const std::string expected = FrameBytes(std::string(20000, 'a')) + std::string(90000, 'b');
        CHECK(ReadRaw(c, expected.size()) == expected);
        CHECK(c.WaitClosed());
        c.Close();
    }
    CHECK(rec.WaitClosed(0).reason == CloseReason::LocalClose);
    rec.CheckInvariants();
}

// 层在握手完成之后出错：OnClosed 报 TlsError，sys_errno 为 0（层的底层错误码只进日志）。
void TestLayerErrorAfterReady()
{
    const LeakCheck leak;
    LServer s(FakeLayerMode::FailAfter, 20);
    {
        TestClient c(s.server.endpoint());
        LayerHandshake(c);
        s.rec.WaitConnected(0);
        SendPlainFrame(c, "0123456789abcdef"); // 4 + 16 = 20 个明文字节，到点报错
        CHECK(c.WaitClosed(5s));
    }
    const auto closed = s.rec.WaitClosed(0);
    CHECK(closed.reason == CloseReason::TlsError && closed.err == 0);
    CHECK(s.stats().closed[static_cast<std::size_t>(CloseReason::TlsError)] == 1);
    for (const auto n : s.stats().handshake_failed)
        CHECK(n == 0); // 握手已完成，不算握手失败
    s.rec.CheckInvariants();
}

// 层在握手期报错：没有 OnConnected 也没有 OnClosed，记 handshake_failed[Tls]，连接被关掉。
void TestLayerErrorDuringHandshake()
{
    const LeakCheck leak;
    LServer s(FakeLayerMode::FailHandshake);
    {
        TestClient c(s.server.endpoint());
        c.SendAll(B("HELO"));
        CHECK(c.WaitClosed(5s));
    }
    CHECK(WaitUntil([&] { return s.HsFailed(HandshakeFailure::Tls) == 1; }));
    CHECK(s.rec.connected_count() == 0 && s.rec.closed_count() == 0);
    CHECK(s.stats().handshaking_connections == 0);
    CHECK(WaitUntil([&] { return s.layer->counters.destroyed == 1; }));
}

void TestLayerHandshakeTimeout()
{
    const LeakCheck leak;
    LServer s(FakeLayerMode::NeverDone, 0, nullptr, 200ms);
    {
        TestClient c(s.server.endpoint());
        c.SendAll(B("HELO"));
        CHECK(c.WaitClosed(5s));
    }
    CHECK(WaitUntil([&] { return s.HsFailed(HandshakeFailure::Timeout) == 1; }));
    CHECK(s.rec.connected_count() == 0 && s.rec.closed_count() == 0);
}

void TestPeerDisconnectMidLayerHandshake()
{
    const LeakCheck leak;
    LServer s;
    {
        TestClient c(s.server.endpoint());
        c.SendAll(B("HE"));
        CHECK(WaitUntil([&] { return s.layer->counters.started == 1; }));
        c.Close();
        CHECK(WaitUntil([&] { return s.HsFailed(HandshakeFailure::Io) == 1; }));
    }
    CHECK(s.rec.connected_count() == 0 && s.rec.closed_count() == 0);
    CHECK(WaitUntil([&] { return s.layer->counters.destroyed == 1; }));
}

// 优雅关闭：写队列排空之后让层发关闭通知（Shutdown），再 shutdown(SHUT_WR)；对端关闭后 OnClosed 是 LocalClose。
void TestCloseGracefullyShutsDownLayer()
{
    const LeakCheck leak;
    LServer s;
    s.rec.on_message = [](IConnection& c, std::string_view sv)
    {
        if (sv == "bye")
            c.CloseGracefully(2s);
        else
            Echo(c, sv);
    };
    {
        TestClient c(s.server.endpoint());
        LayerHandshake(c);
        SendPlainFrame(c, "bye");
        CHECK(c.WaitClosed(5s)); // 读到服务端 shutdown(SHUT_WR) 之后的 EOF
        c.Close();
    }
    const auto closed = s.rec.WaitClosed(0);
    CHECK(closed.reason == CloseReason::LocalClose);
    CHECK(s.layer->counters.shutdowns == 1);
    s.rec.CheckInvariants();
}

// 别的线程经 ConnectionHandle 发送：走任务队列 → SendFromTask（pending_bytes 已预记账）→ 分层写路径。含一个 2MB 的 payload。
void TestCrossThreadSendByHandle()
{
    const LeakCheck leak;
    LServer s;
    s.rec.on_message = nullptr;
    TestClient c(s.server.endpoint());
    LayerHandshake(c);
    const ConnectionHandle h = s.rec.WaitConnected(0);
    constexpr int kSmall = 300;
    constexpr std::size_t kBig = 2u << 20;
    std::thread t(
        [&]
        {
            for (int i = 0; i < kSmall; ++i)
                CHECK(h.Send(Buf("m" + std::to_string(i))) == SendResult::Ok);
            SharedBuffer big = SharedBuffer::Create(kBig);
            const auto m = big.MutableBytes();
            for (std::size_t i = 0; i < kBig; ++i)
                m[i] = static_cast<std::byte>(i * 17 + 3);
            CHECK(h.Send(big) == SendResult::Ok);
            CHECK(h.Send(Buf("end")) == SendResult::Ok);
        });
    for (int i = 0; i < kSmall; ++i)
        CHECK(RecvPlainFrame(c) == "m" + std::to_string(i));
    const std::string big = RecvPlainFrame(c);
    CHECK(big.size() == kBig);
    for (std::size_t i = 0; i < kBig; i += 4093)
        CHECK(static_cast<unsigned char>(big[i]) == static_cast<unsigned char>(i * 17 + 3));
    CHECK(RecvPlainFrame(c) == "end");
    t.join();
}

// PauseRead 之后不再读（层里也不会有解出来的明文被交付）；ResumeRead 之后补上。
void TestPauseAndResumeRead()
{
    const LeakCheck leak;
    LServer s;
    s.rec.on_message = [](IConnection& c, std::string_view sv)
    {
        if (sv == "pause")
            c.PauseRead();
    };
    TestClient c(s.server.endpoint());
    LayerHandshake(c);
    const ConnectionHandle h = s.rec.WaitConnected(0);
    SendPlainFrame(c, "pause");
    CHECK(s.rec.WaitMessages(1));
    SendPlainFrame(c, "after");
    std::this_thread::sleep_for(200ms);
    CHECK(s.rec.message_count() == 1); // 暂停期间没有读，"after" 还躺在内核缓冲里
    h.ResumeRead();
    CHECK(s.rec.WaitMessages(2));
    CHECK(s.rec.payloads()[1] == "after");
}

// ---- 客户端 ----

void TestLayeredClientEchoes()
{
    const LeakCheck leak;
    LServer s;
    LClient cl;
    {
        StreamClient c(cl.group, s.server.endpoint(), cl.Options(), FastPolicy(), cl.rec, *cl.codec);
        CHECK(c.Start() == 0);
        cl.rec.WaitConnected(0);
        CHECK(c.Send(Buf("ping")) == SendResult::Ok);
        CHECK(cl.rec.WaitMessages(1));
        CHECK(cl.rec.payloads()[0] == "ping");
        CHECK(cl.group.GetStats().handshaking_connections == 0);
    }
    cl.rec.CheckInvariants();
    CHECK(cl.layer->counters.created == 1);
}

// 握手必败（服务端层报错、关闭连接）：客户端按连接失败处理，退避会增长并持续重试；errno 是固定映射（不是层的错误码）。
void TestLayeredClientHandshakeFailureBacksOff()
{
    const LeakCheck leak;
    LServer s(FakeLayerMode::FailHandshake);
    LClient cl;

    std::mutex mu;
    std::vector<std::chrono::steady_clock::time_point> times;
    std::vector<std::pair<int, bool>> fails;
    StreamClientOptions co = cl.Options();
    co.on_connect_failed = [&](int e, bool retry) noexcept
    {
        const std::lock_guard lk(mu);
        times.push_back(std::chrono::steady_clock::now());
        fails.emplace_back(e, retry);
    };
    {
        StreamClient c(cl.group, s.server.endpoint(), std::move(co), ReconnectPolicy{40ms, 400ms, 2.0, 0.0}, cl.rec, *cl.codec);
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
        CHECK((e == ECONNRESET || e == EPIPE || e == ECONNABORTED) && retry);
    const auto gap = [&](std::size_t i) { return std::chrono::duration_cast<std::chrono::milliseconds>(times[i + 1] - times[i]); };
    CHECK(gap(3) >= gap(0) + 150ms); // 40 → 320ms 的退避：间隔必须明显变长
    CHECK(cl.rec.connected_count() == 0 && cl.rec.closed_count() == 0);
}

// ---- 层 + codec 的双重握手（wss 的形状）----

// 服务端：层的握手字节与 codec 的握手字节一次到达（同一次 read）。OnConnected 要等两者都完成。
void TestLayerAndCodecHandshakeInSameRead()
{
    const LeakCheck leak;
    LServer s(FakeLayerMode::Normal, 0, std::make_unique<HelloCodecFactory>(true, HelloMode::Normal));
    {
        TestClient c(s.server.endpoint());
        c.SendAll(B(std::string("HELO") + FakeXor("H")));
        CHECK(ReadRaw(c, 4) == "OK!!");
        CHECK(FakeXor(ReadRaw(c, 1)) == "W"); // codec 的应答经过了层（被异或）
        s.rec.WaitConnected(0);
        SendPlainFrame(c, "echo");
        CHECK(RecvPlainFrame(c) == "echo");
    }
    s.rec.WaitClosed(0);
    s.rec.CheckInvariants();
}

// 服务端：层先握手完成，codec 的字节隔一会儿才来：这段时间 OnConnected 不能回调。
void TestLayerDoneBeforeCodecHandshake()
{
    const LeakCheck leak;
    LServer s(FakeLayerMode::Normal, 0, std::make_unique<HelloCodecFactory>(true, HelloMode::Normal));
    {
        TestClient c(s.server.endpoint());
        LayerHandshake(c);
        std::this_thread::sleep_for(150ms);
        CHECK(s.rec.connected_count() == 0); // 层好了，codec 还没握手
        CHECK(s.stats().handshaking_connections == 1);
        c.SendAll(B(FakeXor("H")));
        CHECK(FakeXor(ReadRaw(c, 1)) == "W");
        s.rec.WaitConnected(0);
        CHECK(s.stats().handshaking_connections == 0);
    }
    s.rec.WaitClosed(0);
}

// 客户端：codec 的 Attach 里 SendRaw 握手字节时层还没就绪——这些字节先排进 out_，层就绪后才加密写出；两侧都握手完才 OnConnected。
void TestClientLayerAndCodecHandshakeEndToEnd()
{
    const LeakCheck leak;
    LServer s(FakeLayerMode::Normal, 0, std::make_unique<HelloCodecFactory>(true, HelloMode::Normal));
    LClient cl(std::make_unique<HelloCodecFactory>(false, HelloMode::Normal));
    {
        StreamClient c(cl.group, s.server.endpoint(), cl.Options(), FastPolicy(), cl.rec, *cl.codec);
        CHECK(c.Start() == 0);
        cl.rec.WaitConnected(0);
        s.rec.WaitConnected(0);
        CHECK(c.Send(Buf("both")) == SendResult::Ok);
        CHECK(cl.rec.WaitMessages(1));
        CHECK(cl.rec.payloads()[0] == "both");
    }
    cl.rec.CheckInvariants();
}

// ---- codec 要改写出站 payload（WebSocket 客户端的形状）----

// payload 与 0x33 异或，双向都是；traits().encodes_payload = true。没有层，所以连接用恒等层走分层写路径。
class MaskCodec final : public IFrameCodec
{
public:
    CodecTraits traits() const noexcept override { return {.encodes_payload = true}; }

    DecodeResult Decode(std::span<std::byte> data, FrameSink sink, std::size_t& used) override
    {
        used = 0;
        if (data.size() < 4)
            return DecodeResult::NeedMore;
        const std::size_t len = (static_cast<std::size_t>(data[0]) << 24) | (static_cast<std::size_t>(data[1]) << 16) |
                                (static_cast<std::size_t>(data[2]) << 8) | static_cast<std::size_t>(data[3]);
        if (len > kMaxFrame)
            return DecodeResult::Error;
        if (data.size() - 4 < len)
            return DecodeResult::NeedMore;
        const std::span<std::byte> payload = data.subspan(4, len);
        for (std::byte& b : payload)
            b ^= std::byte{0x33}; // 完整帧才原地改写（契约）
        sink(payload);
        used = 4 + len;
        return DecodeResult::Ok;
    }

    std::size_t EncodeHeader(std::size_t payload_len, std::span<std::byte, kMaxFrameHeaderSize> header_out) override
    {
        header_out[0] = static_cast<std::byte>(payload_len >> 24);
        header_out[1] = static_cast<std::byte>(payload_len >> 16);
        header_out[2] = static_cast<std::byte>(payload_len >> 8);
        header_out[3] = static_cast<std::byte>(payload_len);
        return 4;
    }

    void EncodePayload(std::span<std::byte> payload) noexcept override
    {
        for (std::byte& b : payload)
            b ^= std::byte{0x33};
    }
};

class MaskCodecFactory final : public IFrameCodecFactory
{
public:
    std::unique_ptr<IFrameCodec> Create() const override { return std::make_unique<MaskCodec>(); }
};

std::string Mask(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(c ^ 0x33);
    return s;
}

// 服务端 / 客户端的 codec 都改写 payload：连接走恒等层；出站是拷贝上改写，调用方的原 SharedBuffer 纹丝不动（它可能还要发给别的连接）。
void TestEncodesPayloadUsesIdentityLayerAndLeavesOriginalAlone()
{
    const LeakCheck leak;
    EventRecorder rec;
    MaskCodecFactory codec;
    EventLoopGroup group({1, 64});
    StreamServer server(group, *Endpoint::Tcp("127.0.0.1", 0), {}, rec, codec);
    CHECK(group.Start() == 0);
    CHECK(server.Start() == 0);
    std::atomic<bool> original_untouched{false};
    rec.on_message = [&](IConnection& c, std::string_view sv)
    {
        if (sv == "shared")
        {
            SharedBuffer p = Buf("original");
            CHECK(c.Send(p) == SendResult::Ok);
            CHECK(c.Send(p) == SendResult::Ok); // 同一块发两次：两次都得是被改写的拷贝
            original_untouched = (Str(p.bytes()) == "original");
        }
        else
        {
            Echo(c, sv);
        }
    };
    {
        TestClient c(server.endpoint());
        // 原始 TCP 客户端手工掩码：头是明文长度，payload 与 0x33 异或
        const auto send_masked = [&](std::string_view payload) { c.SendAll(B(FrameBytes(Mask(std::string(payload))))); };
        send_masked("hello");
        CHECK(Mask(c.RecvFrame()) == "hello"); // 回显也是掩码后的
        send_masked("shared");
        CHECK(Mask(c.RecvFrame()) == "original");
        CHECK(Mask(c.RecvFrame()) == "original");
        CHECK(original_untouched.load());
    }
    rec.WaitClosed(0);
    rec.CheckInvariants();
}

// ---- 选项 ----

// 层只用于 TCP：UDS 端点配了它，Start 返回 EPROTONOSUPPORT，不静默忽略。
void TestLayerOnUdsIsRejected()
{
    const LeakCheck leak;
    TempDir dir;
    EventRecorder rec;
    LengthPrefixCodecFactory codec(kMaxFrame);
    EventLoopGroup group({1, 64});
    CHECK(group.Start() == 0);
    const auto layer = std::make_shared<FakeLayerFactory>();
    {
        StreamServerOptions so;
        so.tls = layer;
        StreamServer server(group, dir.Unix("s.sock"), std::move(so), rec, codec);
        CHECK(server.Start() == EPROTONOSUPPORT);
    }
    {
        StreamClientOptions co;
        co.tls = layer;
        StreamClient client(group, dir.Unix("c.sock"), std::move(co), FastPolicy(), rec, codec);
        CHECK(client.Start() == EPROTONOSUPPORT);
    }
    CHECK(layer->counters.created == 0);
}

} // namespace

int main()
{
    TestEchoThroughLayer();
    TestHandshakeBytesSplitByteByByte();
    TestPlaintextInSameReadAsHandshake();
    TestLargePayloadCrossesWireBudget();
    TestBacklogOfManyFramesKeepsOrder();
    TestGracefulCloseFlushesPayloadAndLayerShutdownBytes();
    TestLayerErrorAfterReady();
    TestLayerErrorDuringHandshake();
    TestLayerHandshakeTimeout();
    TestPeerDisconnectMidLayerHandshake();
    TestCloseGracefullyShutsDownLayer();
    TestCrossThreadSendByHandle();
    TestPauseAndResumeRead();
    TestLayeredClientEchoes();
    TestLayeredClientHandshakeFailureBacksOff();
    TestLayerAndCodecHandshakeInSameRead();
    TestLayerDoneBeforeCodecHandshake();
    TestClientLayerAndCodecHandshakeEndToEnd();
    TestEncodesPayloadUsesIdentityLayerAndLeavesOriginalAlone();
    TestLayerOnUdsIsRejected();
    std::puts("layered_connection_test: ok");
    return 0;
}
