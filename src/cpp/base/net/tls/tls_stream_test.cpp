// TLS 端到端：真 socket、真 EventLoop、真 BoringSSL（StreamServer / StreamClient + TlsContext + LengthPrefix 分帧）。
// TlsLayer 本身的行为在 internal/tls_layer_test.cpp（内存里两个层互喂）；这里测连接层与 TLS 的配合：
// 经 TLS 的回显（TLS 1.2 / 1.3，含超过一个 record 的大消息）、**线上确实没有明文**（中继抓包）、明文打到 TLS 端口、
// TLS 打到明文端口、握手超时、证书 / 主机名校验失败后客户端退避重试、mTLS 的时序（TLS 1.3 下客户端先 OnConnected 再被拒）、
// 优雅关闭（close_notify）、PauseRead / ResumeRead、空闲超时、跨线程句柄发送、大量并发连接、热更证书，
// 以及与系统 `openssl s_client` / `s_server` 的真实互通。
// 无测试框架依赖：失败即 abort。

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "base/net/codec/length_prefix_codec.h"
#include "base/net/loop/event_loop_group.h"
#include "base/net/stream/stream_client.h"
#include "base/net/stream/stream_server.h"
#include "base/net/testing/net_test_util.h"
#include "base/net/testing/tls_test_util.h"

using namespace breeze;
using namespace breeze::testing;

namespace
{

constexpr std::size_t kMaxFrame = 16u << 20;

const TestPki& P() { return Pki(); }

// ---- 夹具 ----

struct TlsServer
{
    explicit TlsServer(std::shared_ptr<const IStreamLayerFactory> tls,
                       std::chrono::milliseconds handshake_timeout = 5s,
                       std::chrono::milliseconds idle_timeout = 0ms,
                       std::uint32_t io_threads = 1)
        : group({io_threads, 4096})
        , server(group, *Endpoint::Tcp("127.0.0.1", 0), Options(std::move(tls), handshake_timeout, idle_timeout), rec, codec)
    {
        rec.on_message = Echo;
        CHECK(group.Start() == 0);
        CHECK(server.Start() == 0);
    }
    static StreamServerOptions Options(std::shared_ptr<const IStreamLayerFactory> tls,
                                       std::chrono::milliseconds handshake_timeout,
                                       std::chrono::milliseconds idle_timeout)
    {
        StreamServerOptions o;
        o.tls = std::move(tls);
        o.connection.handshake_timeout = handshake_timeout;
        o.connection.idle_timeout = idle_timeout;
        o.connection.high_water_mark = 64u << 20;
        return o;
    }
    NetStats stats() const { return group.GetStats(); }
    std::uint64_t Failed(HandshakeFailure f) const { return stats().handshake_failed[static_cast<std::size_t>(f)]; }
    const Endpoint& ep() const { return server.endpoint(); }

    EventRecorder rec;
    LengthPrefixCodecFactory codec{kMaxFrame};
    EventLoopGroup group;
    StreamServer server;
};

struct TlsClientSide
{
    explicit TlsClientSide(std::uint32_t io_threads = 1)
        : group({io_threads, 4096})
    {
        CHECK(group.Start() == 0);
    }
    static StreamClientOptions Options(std::shared_ptr<const IStreamLayerFactory> tls)
    {
        StreamClientOptions o;
        o.tls = std::move(tls);
        o.connection.high_water_mark = 64u << 20;
        return o;
    }
    EventRecorder rec;
    LengthPrefixCodecFactory codec{kMaxFrame};
    EventLoopGroup group;
};

ReconnectPolicy FastPolicy() { return {20ms, 100ms, 2.0, 0.0}; }

std::string Pattern(std::size_t n, int seed = 0)
{
    std::string s(n, '\0');
    for (std::size_t i = 0; i < n; ++i)
        s[i] = static_cast<char>('a' + (i * 7 + static_cast<std::size_t>(seed)) % 23);
    return s;
}

// ---- 回显 ----

void TestEchoOverTls()
{
    const LeakCheck leak;
    for (const bool tls12 : {false, true})
    {
        auto so = ServerOpts(P().server);
        auto co = ClientOpts(P().ca);
        if (tls12)
            so.max_version = co.max_version = TlsVersion::Tls12;
        TlsServer s(MustServer(so));
        TlsClientSide cs;
        {
            StreamClient c(cs.group, s.ep(), TlsClientSide::Options(MustClient(co)), FastPolicy(), cs.rec, cs.codec);
            CHECK(c.Start() == 0);
            cs.rec.WaitConnected(0);
            CHECK(c.handle().valid());
            std::size_t sent = 0;
            // 0 与 1 字节、record 边界（16384 ± 1）、跨多个 record、3MB
            for (const std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{100}, std::size_t{16383}, std::size_t{16384},
                                        std::size_t{16385}, std::size_t{70000}, std::size_t{3} << 20})
            {
                const std::string payload = Pattern(n, static_cast<int>(n));
                CHECK(c.Send(Buf(payload)) == SendResult::Ok);
                ++sent;
                CHECK(cs.rec.WaitMessages(sent, 30s));
                CHECK(cs.rec.payloads().back() == payload);
            }
            CHECK(cs.group.GetStats().handshaking_connections == 0 && s.stats().handshaking_connections == 0);
        }
        cs.rec.CheckInvariants();
        s.rec.CheckInvariants();
    }
}

// ---- 线上确实没有明文 ----

// 一个一次性的 TCP 中继：接一条连接、转发到目标，把两个方向的字节都抓下来。
class Relay
{
public:
    explicit Relay(const Endpoint& target)
        : target_(target)
    {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        CHECK(listen_fd_ >= 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(::bind(listen_fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
        CHECK(::listen(listen_fd_, 4) == 0);
        socklen_t len = sizeof(a);
        CHECK(::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&a), &len) == 0);
        port_ = ntohs(a.sin_port);
        thread_ = std::thread([this] { Run(); });
    }
    ~Relay()
    {
        stop_.store(true);
        thread_.join();
        ::close(listen_fd_);
    }
    Endpoint endpoint() const { return *Endpoint::Tcp("127.0.0.1", port_); }
    std::string captured() const
    {
        const std::lock_guard lk(mu_);
        return captured_;
    }

private:
    void Run()
    {
        pollfd lp{listen_fd_, POLLIN, 0};
        int a = -1;
        while (!stop_.load() && a < 0)
            if (::poll(&lp, 1, 50) > 0)
                a = ::accept(listen_fd_, nullptr, nullptr);
        if (a < 0)
            return;
        const int b = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in t{};
        t.sin_family = AF_INET;
        t.sin_port = htons(target_.port());
        ::inet_pton(AF_INET, target_.address().c_str(), &t.sin_addr);
        CHECK(::connect(b, reinterpret_cast<sockaddr*>(&t), sizeof(t)) == 0);
        char buf[64 * 1024];
        while (!stop_.load())
        {
            pollfd fds[2] = {{a, POLLIN, 0}, {b, POLLIN, 0}};
            if (::poll(fds, 2, 50) <= 0)
                continue;
            bool eof = false;
            for (int i = 0; i < 2; ++i)
            {
                if (!(fds[i].revents & (POLLIN | POLLHUP)))
                    continue;
                const ssize_t n = ::recv(fds[i].fd, buf, sizeof(buf), 0);
                if (n <= 0)
                {
                    eof = true;
                    continue;
                }
                {
                    const std::lock_guard lk(mu_);
                    captured_.append(buf, static_cast<std::size_t>(n));
                }
                const int other = i == 0 ? b : a;
                std::size_t off = 0;
                while (off < static_cast<std::size_t>(n))
                {
                    const ssize_t w = ::send(other, buf + off, static_cast<std::size_t>(n) - off, MSG_NOSIGNAL);
                    if (w <= 0)
                    {
                        eof = true;
                        break;
                    }
                    off += static_cast<std::size_t>(w);
                }
            }
            if (eof)
                break;
        }
        ::close(a);
        ::close(b);
    }

    Endpoint target_;
    int listen_fd_ = -1;
    std::uint16_t port_ = 0;
    std::atomic<bool> stop_{false};
    mutable std::mutex mu_;
    std::string captured_;
    std::thread thread_;
};

void TestWireHasNoPlaintext()
{
    const LeakCheck leak;
    TlsServer s(MustServer(ServerOpts(P().server)));
    const std::string secret = "TOP-SECRET-PAYLOAD-0123456789-do-not-leak";
    {
        Relay relay(s.ep());
        TlsClientSide cs;
        StreamClient c(cs.group, relay.endpoint(), TlsClientSide::Options(MustClient(ClientOpts(P().ca))), FastPolicy(), cs.rec, cs.codec);
        CHECK(c.Start() == 0);
        cs.rec.WaitConnected(0);
        CHECK(c.Send(Buf(secret)) == SendResult::Ok);
        CHECK(cs.rec.WaitMessages(1));
        CHECK(cs.rec.payloads()[0] == secret); // 回显回来了，说明确实走通了
        const std::string wire = relay.captured();
        CHECK(wire.size() > secret.size() * 2);                       // 两个方向都有字节
        CHECK(wire.find(secret) == std::string::npos);                // 但没有明文
        CHECK(wire.find("TOP-SECRET") == std::string::npos);
        CHECK(wire.size() >= 3 && static_cast<unsigned char>(wire[0]) == 0x16); // 第一个字节是 TLS 握手 record 的类型
    }
}

// ---- 握手失败 ----

void TestPlaintextClientIsRejected()
{
    const LeakCheck leak;
    TlsServer s(MustServer(ServerOpts(P().server)));
    {
        TestClient c(s.ep());
        c.SendFrame("this is a plaintext frame, not a ClientHello"); // 明文的 LengthPrefix 帧
        CHECK(c.WaitClosed(5s));
        c.Close();
    }
    CHECK(WaitUntil([&] { return s.Failed(HandshakeFailure::Tls) == 1; }));
    CHECK(s.rec.connected_count() == 0 && s.rec.closed_count() == 0);
    CHECK(s.stats().handshaking_connections == 0);
}

void TestTlsClientToPlainServerFails()
{
    const LeakCheck leak;
    // 明文服务端（没有配 tls）：把 ClientHello 当成 LengthPrefix 帧，长度字段超限 → 关闭
    EventRecorder srec;
    LengthPrefixCodecFactory codec{kMaxFrame};
    EventLoopGroup sgroup({1, 64});
    StreamServer server(sgroup, *Endpoint::Tcp("127.0.0.1", 0), {}, srec, codec);
    CHECK(sgroup.Start() == 0 && server.Start() == 0);

    TlsClientSide cs;
    std::mutex mu;
    std::vector<std::pair<int, bool>> fails;
    auto co = TlsClientSide::Options(MustClient(ClientOpts(P().ca)));
    co.on_connect_failed = [&](int e, bool retry) noexcept
    {
        const std::lock_guard lk(mu);
        fails.emplace_back(e, retry);
    };
    {
        StreamClient c(cs.group, server.endpoint(), std::move(co), FastPolicy(), cs.rec, cs.codec);
        CHECK(c.Start() == 0);
        CHECK(WaitUntil(
            [&]
            {
                const std::lock_guard lk(mu);
                return fails.size() >= 3;
            }));
    }
    const std::lock_guard lk(mu);
    for (const auto& [e, retry] : fails)
        CHECK(e != 0 && retry);
    // 客户端：握手没完成，没有任何回调。明文服务端没有握手阶段，TCP 一接上就 OnConnected，随后把 ClientHello 当成超长的帧以
    // ProtocolError 关闭——每次重试都是这样。
    CHECK(cs.rec.connected_count() == 0 && cs.rec.closed_count() == 0);
    CHECK(WaitUntil([&] { return srec.closed_count() >= 3; }));
    CHECK(srec.WaitClosed(0).reason == CloseReason::ProtocolError);
}

void TestHandshakeTimeout()
{
    const LeakCheck leak;
    TlsServer s(MustServer(ServerOpts(P().server)), 200ms);
    {
        TestClient c(s.ep());
        const std::byte partial[3] = {std::byte{0x16}, std::byte{0x03}, std::byte{0x01}}; // ClientHello 的头只发了一半
        c.SendAll(partial);
        CHECK(WaitUntil([&] { return s.stats().handshaking_connections == 1; }));
        CHECK(c.WaitClosed(5s));
    }
    CHECK(WaitUntil([&] { return s.Failed(HandshakeFailure::Timeout) == 1; }));
    CHECK(s.rec.connected_count() == 0 && s.rec.closed_count() == 0);
}

// 证书 / 主机名校验失败：客户端按握手失败处理——退避重试、on_connect_failed 收到固定的 EPROTO、没有 OnConnected；
// 服务端把它记成握手失败（Tls）。
void TestVerificationFailureBacksOffAndRetries()
{
    const LeakCheck leak;
    struct Case
    {
        const char* what;
        TlsClientOptions opts;
    };
    const std::vector<Case> cases = {{"主机名不对", ClientOpts(P().ca, "wrong.name")}, {"不信任签发者", ClientOpts(P().other_ca)}};
    for (const Case& k : cases)
    {
        TlsServer s(MustServer(ServerOpts(P().server)));
        TlsClientSide cs;
        std::mutex mu;
        std::vector<std::pair<int, bool>> fails;
        auto co = TlsClientSide::Options(MustClient(k.opts));
        co.on_connect_failed = [&](int e, bool retry) noexcept
        {
            const std::lock_guard lk(mu);
            fails.emplace_back(e, retry);
        };
        {
            StreamClient c(cs.group, s.ep(), std::move(co), FastPolicy(), cs.rec, cs.codec);
            CHECK(c.Start() == 0);
            CHECK(WaitUntil(
                [&]
                {
                    const std::lock_guard lk(mu);
                    return fails.size() >= 3;
                }));
        }
        {
            const std::lock_guard lk(mu);
            for (const auto& [e, retry] : fails)
                CHECK(e == EPROTO && retry); // 固定映射的 errno，不是 TLS 库的错误码
        }
        CHECK(cs.rec.connected_count() == 0 && cs.rec.closed_count() == 0 && s.rec.connected_count() == 0);
        CHECK(cs.group.GetStats().handshake_failed[static_cast<std::size_t>(HandshakeFailure::Tls)] >= 3);
        // 服务端也记握手失败：客户端校验失败后直接关连接，服务端读到的是 EOF（记 Io），读到 alert 才记 Tls——两种都算。
        CHECK(WaitUntil([&] { return s.Failed(HandshakeFailure::Tls) + s.Failed(HandshakeFailure::Io) >= 3; }));
        (void)k.what;
    }
}

// mTLS：带了证书正常；要求证书但没带——TLS 1.3 下客户端发完 Finished 就算完成，所以客户端先 OnConnected、随后被 alert 关闭
// （TlsError）；服务端没有 OnConnected，只记握手失败。
void TestMutualTlsOverStreams()
{
    const LeakCheck leak;
    auto so = ServerOpts(P().server);
    so.client_ca.pem = P().ca.cert_pem;
    so.require_client_cert = true;
    TlsServer s(MustServer(so));
    {
        auto co = ClientOpts(P().ca);
        co.cert_chain.pem = P().client.cert_pem;
        co.private_key.pem = P().client.key_pem;
        TlsClientSide cs;
        StreamClient c(cs.group, s.ep(), TlsClientSide::Options(MustClient(co)), FastPolicy(), cs.rec, cs.codec);
        CHECK(c.Start() == 0);
        cs.rec.WaitConnected(0);
        CHECK(c.Send(Buf("with-cert")) == SendResult::Ok);
        CHECK(cs.rec.WaitMessages(1));
        CHECK(cs.rec.payloads()[0] == "with-cert");
    }
    {
        const auto before = s.Failed(HandshakeFailure::Tls);
        TlsClientSide cs;
        StreamClient c(cs.group, s.ep(), TlsClientSide::Options(MustClient(ClientOpts(P().ca))), FastPolicy(), cs.rec, cs.codec);
        CHECK(c.Start() == 0);
        const auto closed = cs.rec.WaitClosed(0); // 客户端先 OnConnected（握手"完成"），随后被服务端的 alert 关闭
        CHECK(closed.reason == CloseReason::TlsError);
        CHECK(cs.rec.connected_count() >= 1);
        CHECK(WaitUntil([&] { return s.Failed(HandshakeFailure::Tls) > before; }));
        c.Stop();
    }
}

// ---- 连接层功能经 TLS ----

void TestGracefulCloseOverTls()
{
    const LeakCheck leak;
    TlsServer s(MustServer(ServerOpts(P().server)));
    s.rec.on_message = [](IConnection& c, std::string_view m)
    {
        if (m == "bye")
            c.CloseGracefully(2s);
        else
            Echo(c, m);
    };
    TlsClientSide cs;
    {
        StreamClient c(cs.group, s.ep(), TlsClientSide::Options(MustClient(ClientOpts(P().ca))), FastPolicy(), cs.rec, cs.codec);
        CHECK(c.Start() == 0);
        cs.rec.WaitConnected(0);
        CHECK(c.Send(Buf("hello")) == SendResult::Ok);
        CHECK(cs.rec.WaitMessages(1));
        CHECK(c.Send(Buf("bye")) == SendResult::Ok);
        const auto cc = cs.rec.WaitClosed(0);
        CHECK(cc.reason == CloseReason::PeerClosed); // 收到服务端的 close_notify（PeerShutdown）
        const auto sc = s.rec.WaitClosed(0);
        CHECK(sc.reason == CloseReason::LocalClose);
        c.Stop();
    }
}

// PauseRead 之后不再读（TLS 层里也不会有解出来的明文被交付）；经句柄 ResumeRead 之后补上。
void TestPauseResumeOverTls()
{
    const LeakCheck leak;
    TlsServer s(MustServer(ServerOpts(P().server)));
    s.rec.on_message = [](IConnection& c, std::string_view m)
    {
        if (m == "pause")
            c.PauseRead();
        Echo(c, m);
    };
    TlsClientSide cs;
    {
        StreamClient c(cs.group, s.ep(), TlsClientSide::Options(MustClient(ClientOpts(P().ca))), FastPolicy(), cs.rec, cs.codec);
        CHECK(c.Start() == 0);
        cs.rec.WaitConnected(0);
        const ConnectionHandle sh = s.rec.WaitConnected(0);
        CHECK(c.Send(Buf("pause")) == SendResult::Ok);
        CHECK(cs.rec.WaitMessages(1));
        for (int i = 0; i < 5; ++i)
            CHECK(c.Send(Buf("after-" + std::to_string(i))) == SendResult::Ok);
        std::this_thread::sleep_for(300ms);
        CHECK(cs.rec.message_count() == 1); // 暂停期间服务端没有读：后面的几条还躺在内核缓冲里
        CHECK(s.stats().paused_connections == 1);
        sh.ResumeRead();
        CHECK(cs.rec.WaitMessages(6));
        for (int i = 0; i < 5; ++i)
            CHECK(cs.rec.payloads()[static_cast<std::size_t>(i) + 1] == "after-" + std::to_string(i));
    }
}

// idle_timeout：握手之后一直没有字节就以 IdleTimeout 关闭（握手阶段由 handshake_timeout 管，不归它）。
void TestIdleTimeoutOverTls()
{
    const LeakCheck leak;
    TlsServer s(MustServer(ServerOpts(P().server)), 5s, 300ms);
    TlsClientSide cs;
    {
        StreamClient c(cs.group, s.ep(), TlsClientSide::Options(MustClient(ClientOpts(P().ca))), FastPolicy(), cs.rec, cs.codec);
        CHECK(c.Start() == 0);
        cs.rec.WaitConnected(0);
        const auto closed = s.rec.WaitClosed(0, 10s);
        CHECK(closed.reason == CloseReason::IdleTimeout);
        c.Stop();
    }
}

// 多个线程经句柄并发 Send：全部到达、每个线程自己的顺序不乱。
void TestCrossThreadSendOverTls()
{
    const LeakCheck leak;
    TlsServer s(MustServer(ServerOpts(P().server)));
    TlsClientSide cs;
    constexpr int kThreads = 4;
    constexpr int kEach = 300;
    {
        StreamClient c(cs.group, s.ep(), TlsClientSide::Options(MustClient(ClientOpts(P().ca))), FastPolicy(), cs.rec, cs.codec);
        CHECK(c.Start() == 0);
        cs.rec.WaitConnected(0);
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t)
            threads.emplace_back(
                [&, t]
                {
                    for (int i = 0; i < kEach; ++i)
                        CHECK(c.Send(Buf("t" + std::to_string(t) + "-" + std::to_string(i) + std::string(static_cast<std::size_t>(i % 40), 'x'))) ==
                              SendResult::Ok);
                });
        for (auto& th : threads)
            th.join();
        CHECK(cs.rec.WaitMessages(kThreads * kEach, 30s));
        std::vector<int> next(kThreads, 0);
        for (const std::string& m : cs.rec.payloads())
        {
            const int t = m[1] - '0';
            const int i = std::stoi(m.substr(3));
            CHECK(i == next[static_cast<std::size_t>(t)]++); // 同一线程的消息按发送顺序到达
        }
    }
}

// 很多连接同时握手、收发、断开（服务端 2 个 IO 线程）。
void TestManyConcurrentConnections()
{
    const LeakCheck leak;
    constexpr int kN = 150;
    TlsServer s(MustServer(ServerOpts(P().server)), 10s, 0ms, 2);
    TlsClientSide cs(2);
    cs.rec.on_connected = [](IConnection& c) { Echo(c, "hi"); };
    const auto cctx = MustClient(ClientOpts(P().ca));
    {
        std::vector<std::unique_ptr<StreamClient>> clients;
        for (int i = 0; i < kN; ++i)
        {
            clients.push_back(std::make_unique<StreamClient>(cs.group, s.ep(), TlsClientSide::Options(cctx), FastPolicy(), cs.rec, cs.codec));
            CHECK(clients.back()->Start() == 0);
        }
        CHECK(cs.rec.WaitMessages(kN, 30s)); // 每条连接 OnConnected 里发一条、服务端回显
        CHECK(WaitUntil([&] { return s.stats().handshaking_connections == 0 && cs.group.GetStats().handshaking_connections == 0; }));
        CHECK(s.rec.connected_count() == kN);
    }
    CHECK(WaitUntil([&] { return s.rec.closed_count() == kN; }, 20s));
    s.rec.CheckInvariants();
}

// 热更证书：新建的连接用新证书，已建立的连接不受影响。第二个客户端只信任第二个 CA：重载之前握手失败（持续退避重试），重载之后连上。
void TestCertificateReload()
{
    const LeakCheck leak;
    const TestCert cert1 = MakeCert({.cn = "server.test", .dns = {"server.test"}, .issuer = &P().ca});
    const TestCert cert2 = MakeCert({.cn = "server.test", .dns = {"server.test"}, .issuer = &P().other_ca});
    auto holder = std::make_shared<TlsContextHolder>(MustServer(ServerOpts(cert1)));
    TlsServer s(holder);
    TlsClientSide cs;
    TlsClientSide cs1;
    {
        // 信任第一个 CA 的客户端：现在能连
        StreamClient old_client(cs1.group, s.ep(), TlsClientSide::Options(MustClient(ClientOpts(P().ca))), FastPolicy(), cs1.rec, cs1.codec);
        CHECK(old_client.Start() == 0);
        cs1.rec.WaitConnected(0);

        // 信任第二个 CA 的客户端：重载之前连不上
        std::atomic<int> failures{0};
        auto co = TlsClientSide::Options(MustClient(ClientOpts(P().other_ca)));
        co.on_connect_failed = [&](int, bool) noexcept { failures.fetch_add(1); };
        StreamClient new_client(cs.group, s.ep(), std::move(co), FastPolicy(), cs.rec, cs.codec);
        CHECK(new_client.Start() == 0);
        CHECK(WaitUntil([&] { return failures.load() >= 2; }));
        CHECK(cs.rec.connected_count() == 0);

        holder->Reload(MustServer(ServerOpts(cert2)));
        cs.rec.WaitConnected(0, 20s); // 重载之后，重连的握手用新证书、通过了校验
        CHECK(new_client.Send(Buf("via new cert")) == SendResult::Ok);
        CHECK(cs.rec.WaitMessages(1));

        // 已建立的旧连接不受影响，仍能收发
        CHECK(old_client.Send(Buf("old connection still works")) == SendResult::Ok);
        CHECK(cs1.rec.WaitMessages(1));
        CHECK(cs1.rec.payloads()[0] == "old connection still works");
        old_client.Stop();
        new_client.Stop();
    }
}

// 对端先关（发 close_notify）：服务端按 RFC 回一个自己的 close_notify 再关，客户端据此知道是正常结束、不是被截断。
// 用一个会读回应的阻塞 BoringSSL 客户端：发完 close_notify 之后再读，应当得到 SSL_ERROR_ZERO_RETURN（收到了对端的 close_notify），
// 而不是 EOF 报错。（openssl s_client 在 stdin 到头发完 close_notify 就退出、不读回应，测不出这件事。）
void TestServerRespondsToCloseNotify()
{
    const LeakCheck leak;
    TlsServer s(MustServer(ServerOpts(P().server)));

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(s.ep().port());
    ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    CHECK(::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);

    SSL_CTX* ctx = SSL_CTX_new(TLS_method());
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    SSL* ssl = SSL_new(ctx);
    CHECK(SSL_set_fd(ssl, fd) == 1);
    CHECK(SSL_connect(ssl) == 1);

    const unsigned char frame[] = {0, 0, 0, 2, 'h', 'i'}; // LengthPrefix 的一帧
    CHECK(SSL_write(ssl, frame, sizeof(frame)) == static_cast<int>(sizeof(frame)));
    unsigned char buf[64];
    CHECK(SSL_read(ssl, buf, 6) == 6); // 回显（途中会透明地处理 TLS 1.3 的 NewSessionTicket）
    CHECK(SSL_shutdown(ssl) == 0);     // 发出 close_notify
    const int n = SSL_read(ssl, buf, sizeof(buf));
    CHECK(n <= 0 && SSL_get_error(ssl, n) == SSL_ERROR_ZERO_RETURN); // 收到服务端回的 close_notify：干净关闭

    SSL_free(ssl);
    SSL_CTX_free(ctx);
    ::close(fd);
}

// ---- 与系统 openssl 命令行互通 ----

struct TempPems
{
    TempPems()
    {
        char tmpl[] = "/tmp/breeze-tls-interop-XXXXXX";
        const char* d = ::mkdtemp(tmpl);
        CHECK(d != nullptr);
        dir = d;
        Write("ca.pem", P().ca.cert_pem);
        Write("other_ca.pem", P().other_ca.cert_pem);
        Write("server.pem", P().server.cert_pem);
        Write("server.key", P().server.key_pem);
    }
    ~TempPems()
    {
        for (const char* n : {"ca.pem", "other_ca.pem", "server.pem", "server.key"})
            ::unlink((dir + "/" + n).c_str());
        ::rmdir(dir.c_str());
    }
    void Write(const std::string& name, const std::string& text) const
    {
        std::FILE* f = std::fopen((dir + "/" + name).c_str(), "wb");
        CHECK(f != nullptr);
        CHECK(std::fwrite(text.data(), 1, text.size(), f) == text.size());
        std::fclose(f);
    }
    std::string dir;
};

std::string RunShell(const std::string& cmd)
{
    std::FILE* p = ::popen(cmd.c_str(), "r");
    CHECK(p != nullptr);
    std::string out;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0)
        out.append(buf, n);
    ::pclose(p);
    return out;
}

bool HaveOpenssl() { return ::access("/usr/bin/openssl", X_OK) == 0; }

// openssl s_client 作客户端连我们的服务端：握手、校验服务端证书（含主机名）、收发 LengthPrefix 帧。
void TestInteropOpenSslClient()
{
    if (!HaveOpenssl())
    {
        std::puts("TestInteropOpenSslClient: 没有 /usr/bin/openssl，跳过");
        return;
    }
    const LeakCheck leak;
    const TempPems pems;
    TlsServer s(MustServer(ServerOpts(P().server)));
    const std::string port = std::to_string(s.ep().port());
    // popen 用 /bin/sh（Ubuntu 上是 dash）：它的 printf 不认 \xHH，只认八进制。
    // -quiet 隐含 -ign_eof（stdin 到头也不关连接，只等对端先关），所以要 -no_ign_eof：否则对着谁都要等满超时。
    const std::string common = "cd " + pems.dir + " && (printf '\\000\\000\\000\\005hello'; sleep 1) | timeout 20 openssl s_client -connect 127.0.0.1:" +
                               port + " -servername server.test -verify_hostname server.test -verify_return_error -quiet -no_ign_eof ";
    const std::string expect("\0\0\0\5hello", 9);
    for (const char* version : {"-tls1_3", "-tls1_2"})
    {
        const std::string out = RunShell(common + "-CAfile ca.pem " + version + " 2>/dev/null | head -c 9");
        if (out != expect) // 失败时把 openssl 自己说的话打出来，排查用
            std::fprintf(stderr, "openssl s_client %s 没有拿到回显（得到 %zu 字节）。它的输出：\n%s\n", version, out.size(),
                         RunShell(common + "-CAfile ca.pem " + version + " 2>&1 | head -c 1500").c_str());
        CHECK(out == expect); // 握手成功、证书与主机名通过校验、帧经 TLS 往返
    }
    CHECK(WaitUntil([&] { return s.rec.connected_count() == 2; }));

    // 不信任的 CA：openssl 校验失败、不发数据；服务端记握手失败（TLS 1.3 下 s_client 校验失败发 alert，不会让服务端完成握手）
    const auto failed_before = s.Failed(HandshakeFailure::Tls);
    const std::string bad = RunShell(common + "-CAfile other_ca.pem 2>/dev/null | head -c 9");
    CHECK(bad.empty());
    CHECK(WaitUntil([&] { return s.Failed(HandshakeFailure::Tls) > failed_before; }));
    CHECK(s.rec.connected_count() == 2); // 只有前面两条成功的连上了
}

// 我们的客户端连 openssl s_server：握手、校验 s_server 的证书与主机名。
void TestInteropOpenSslServer()
{
    if (!HaveOpenssl())
    {
        std::puts("TestInteropOpenSslServer: 没有 /usr/bin/openssl，跳过");
        return;
    }
    const LeakCheck leak;
    const TempPems pems;

    // 找一个空闲端口（关掉再给 s_server 用：有一点点竞态，可接受）
    const int probe = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(::bind(probe, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
    socklen_t len = sizeof(a);
    CHECK(::getsockname(probe, reinterpret_cast<sockaddr*>(&a), &len) == 0);
    const std::uint16_t port = ntohs(a.sin_port);
    ::close(probe);

    int stdin_pipe[2];
    CHECK(::pipe(stdin_pipe) == 0); // 让 s_server 的 stdin 一直开着（EOF 会让它退出）
    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0)
    {
        ::dup2(stdin_pipe[0], 0);
        ::close(stdin_pipe[0]);
        ::close(stdin_pipe[1]);
        // 它的日志不进测试输出：我们的客户端 Stop() 是硬关闭（没有 close_notify），s_server 会为此打一行 "unexpected eof"
        const int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0)
        {
            ::dup2(devnull, 1);
            ::dup2(devnull, 2);
            ::close(devnull);
        }
        if (::chdir(pems.dir.c_str()) != 0)
            ::_exit(126);
        const std::string p = std::to_string(port);
        ::execlp("openssl", "openssl", "s_server", "-accept", p.c_str(), "-cert", "server.pem", "-key", "server.key", "-naccept", "50", "-quiet",
                 static_cast<char*>(nullptr));
        ::_exit(127);
    }
    ::close(stdin_pipe[0]);

    {
        TlsClientSide cs;
        StreamClient c(cs.group, *Endpoint::Tcp("127.0.0.1", port), TlsClientSide::Options(MustClient(ClientOpts(P().ca))), FastPolicy(), cs.rec,
                       cs.codec);
        CHECK(c.Start() == 0);
        cs.rec.WaitConnected(0, 20s); // s_server 起来之前客户端会连不上、退避重试；起来之后握手通过
        c.Stop();
    }
    ::kill(pid, SIGTERM);
    int status = 0;
    ::waitpid(pid, &status, 0);
    ::close(stdin_pipe[1]);
}

} // namespace

// 每个用例的耗时超过 1 秒就打出来：这套测试有真握手、有等待，要盯着别悄悄变慢。
#define RUN(test)                                                                                                      \
    do                                                                                                                 \
    {                                                                                                                  \
        const auto t0 = std::chrono::steady_clock::now();                                                              \
        test();                                                                                                        \
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);   \
        if (ms > 1000ms)                                                                                               \
            std::fprintf(stderr, "[耗时] %s %lld ms\n", #test, static_cast<long long>(ms.count()));                      \
    } while (0)

int main()
{
    ::signal(SIGPIPE, SIG_IGN);
    RUN(TestEchoOverTls);
    RUN(TestWireHasNoPlaintext);
    RUN(TestPlaintextClientIsRejected);
    RUN(TestTlsClientToPlainServerFails);
    RUN(TestHandshakeTimeout);
    RUN(TestVerificationFailureBacksOffAndRetries);
    RUN(TestMutualTlsOverStreams);
    RUN(TestGracefulCloseOverTls);
    RUN(TestServerRespondsToCloseNotify);
    RUN(TestPauseResumeOverTls);
    RUN(TestIdleTimeoutOverTls);
    RUN(TestCrossThreadSendOverTls);
    RUN(TestManyConcurrentConnections);
    RUN(TestCertificateReload);
    RUN(TestInteropOpenSslClient);
    RUN(TestInteropOpenSslServer);
    std::puts("tls_stream_test: ok");
    return 0;
}
