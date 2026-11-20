// TlsLayer：两个层在内存里互相喂字节，不开 socket，不碰连接层。
// 覆盖：TLS 1.2 / 1.3 握手与双向数据；任意切分（逐字节 / 随机块 / 多条 record 合并进一次 Unwrap）；大 payload（3MB）与大量小消息；
// 握手完成与明文同一次 Unwrap 里到达（层契约）；close_notify；明文 / 垃圾打到 TLS 端点；被篡改的 record；
// 证书校验（CA、主机名、IP SAN、过期、verify_peer=false）；mTLS（要求 / 不要求 / 证书不对）；
// session ticket 恢复（服务端 SSL_SESS_CACHE_OFF 下仍成功，以及关掉 ticket / 关掉客户端恢复时不复用）；
// 错误队列的线程本地污染（一条连接的握手错误不能影响同一线程上的另一条）；上下文创建的各种错误；热更证书。
// 无测试框架依赖：失败即 abort。

#include <malloc.h>
#include <openssl/ssl.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "base/net/tls/internal/tls_layer.h"
#include "base/net/tls/tls_context.h"
#include "base/net/testing/tls_test_util.h"

using namespace breeze;
using namespace breeze::testing;

namespace
{

struct Rng
{
    std::uint64_t s;
    std::uint64_t operator()()
    {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    std::size_t Below(std::size_t n) { return static_cast<std::size_t>((*this)() % n); }
};

// PKI 与上下文构造在 testing/tls_test_util.h（与 tls_stream_test 共用）。
const TestPki& P() { return Pki(); }

std::unique_ptr<LayerPair> Pair(const IStreamLayerFactory& server, const IStreamLayerFactory& client)
{
    return std::make_unique<LayerPair>(client.Create(LayerRole::Client), server.Create(LayerRole::Server));
}

tls::TlsLayer& L(LayerEnd& e) { return *static_cast<tls::TlsLayer*>(e.layer.get()); }

// 握手成功：两端各恰好报一次 HandshakeDone、都没出错。
void ExpectHandshakeOk(LayerPair& p)
{
    CHECK(!p.client.errored && !p.server.errored);
    CHECK(p.client.handshake_done_count == 1 && p.server.handshake_done_count == 1);
    CHECK(L(p.client).handshake_done() && L(p.server).handshake_done());
}

// 握手失败：没有任何一端报握手完成，至少一端出错；出错的一端有底层错误码。
void ExpectHandshakeFails(LayerPair& p, int line = __builtin_LINE())
{
    if (p.client.handshake_done_count != 0 || p.server.handshake_done_count != 0)
        std::fprintf(stderr, "第 %d 行：握手本应失败：client done=%d errored=%d，server done=%d errored=%d\n", line,
                     p.client.handshake_done_count, p.client.errored, p.server.handshake_done_count, p.server.errored);
    CHECK(p.client.handshake_done_count == 0 && p.server.handshake_done_count == 0);
    CHECK(p.client.errored || p.server.errored);
    if (p.client.errored)
        CHECK(p.client.layer->error_code() != 0);
    if (p.server.errored)
        CHECK(p.server.layer->error_code() != 0);
}

// 服务端拒绝了客户端证书（mTLS）：服务端没有完成握手、出错。**TLS 1.3 里客户端在服务端校验它的证书之前就已经完成握手**
// （客户端发完 Certificate + Finished 即完成），所以客户端一侧可能先报 HandshakeDone、随后收到 alert 才出错——这不是缺陷，
// 是协议如此（见 tls_context.h 的说明）。
void ExpectServerRejectsClient(LayerPair& p, int line = __builtin_LINE())
{
    if (p.server.handshake_done_count != 0 || !p.server.errored || !p.client.errored)
        std::fprintf(stderr, "第 %d 行：server done=%d errored=%d，client done=%d errored=%d\n", line,
                     p.server.handshake_done_count, p.server.errored, p.client.handshake_done_count, p.client.errored);
    CHECK(p.server.handshake_done_count == 0 && p.server.errored && p.server.layer->error_code() != 0);
    CHECK(p.client.errored); // 服务端的 alert 最终会让客户端出错（TLS 1.3 下可能晚于它自己的 HandshakeDone）
}

// ---- 握手与数据 ----

void TestHandshakeAndDataBothDirections()
{
    for (const bool tls12 : {false, true})
    {
        auto so = ServerOpts(P().server);
        auto co = ClientOpts(P().ca);
        if (tls12)
            so.max_version = co.max_version = TlsVersion::Tls12;
        const auto sc = MustServer(so);
        const auto cc = MustClient(co);
        auto p = Pair(*sc, *cc);
        p->Run();
        ExpectHandshakeOk(*p);
        CHECK(L(p->client).protocol_version() == (tls12 ? 0x0303 : 0x0304));
        CHECK(L(p->server).protocol_version() == (tls12 ? 0x0303 : 0x0304));
        CHECK(L(p->client).peer_subject().find("server.test") != std::string::npos);

        CHECK(p->client.Send("hello server"));
        p->Run();
        CHECK(p->server.plain == "hello server");
        CHECK(p->server.Send("hello client"));
        p->Run();
        CHECK(p->client.plain == "hello client");
        CHECK(p->client.handshake_done_count == 1 && p->server.handshake_done_count == 1); // 之后不会再报
    }
}

// 握手阶段与数据阶段都与切分无关：逐字节、各种固定块、随机块。
void TestChunkingInvariance()
{
    const auto sc = MustServer(ServerOpts(P().server));
    const auto cc = MustClient(ClientOpts(P().ca));
    std::string msg(5000, '\0');
    for (std::size_t i = 0; i < msg.size(); ++i)
        msg[i] = static_cast<char>(i * 31 + 7);

    for (const std::size_t chunk : {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{7}, std::size_t{64}, std::size_t{1000}})
    {
        auto p = Pair(*sc, *cc);
        p->Run(chunk);
        ExpectHandshakeOk(*p);
        CHECK(p->client.Send(msg) && p->server.Send(msg));
        p->Run(chunk);
        CHECK(p->server.plain == msg && p->client.plain == msg);
    }
    Rng rng{0x2545F4914F6CDD1Dull};
    for (int iter = 0; iter < 20; ++iter)
    {
        auto p = Pair(*sc, *cc);
        for (int i = 0; i < 1000 && !p->client.errored && !p->server.errored; ++i)
            if (!p->Pump(1 + rng.Below(40)))
                break;
        ExpectHandshakeOk(*p);
        CHECK(p->client.Send(msg));
        while (p->Pump(1 + rng.Below(100)))
        {
        }
        CHECK(p->server.plain == msg);
    }
}

// 一次 Unwrap 里装着多条 record（写合并 / 积压）：全部解出来。
void TestManyRecordsInOneUnwrap()
{
    const auto sc = MustServer(ServerOpts(P().server));
    const auto cc = MustClient(ClientOpts(P().ca));
    auto p = Pair(*sc, *cc);
    p->Run();
    ExpectHandshakeOk(*p);
    std::string all;
    for (int i = 0; i < 200; ++i)
    {
        const std::string m = "message-" + std::to_string(i);
        all += m;
        CHECK(p->client.Send(m)); // 每次 Wrap 一个 record，200 个 record 攒在 PendingWire 里
    }
    const auto wire = p->client.TakeWire();
    CHECK(wire.size() > 200 * 20);
    p->server.Feed(wire); // 一次 Unwrap
    CHECK(p->server.plain == all);
}

void TestLargePayloadAndManySmallMessages()
{
    const auto sc = MustServer(ServerOpts(P().server));
    const auto cc = MustClient(ClientOpts(P().ca));
    {
        auto p = Pair(*sc, *cc);
        p->Run();
        std::string big(3 * 1024 * 1024, '\0');
        Rng rng{12345};
        for (auto& c : big)
            c = static_cast<char>(rng());
        CHECK(p->client.Send(big));
        p->Run();
        CHECK(p->server.plain == big);
        CHECK(p->server.Send(big));
        p->Run(4096);
        CHECK(p->client.plain == big);
    }
    {
        auto p = Pair(*sc, *cc);
        p->Run();
        Rng rng{777};
        std::string expect_server;
        std::string expect_client;
        for (int i = 0; i < 5000; ++i)
        {
            const std::string m(rng.Below(200), static_cast<char>('a' + i % 26));
            if (i % 2 == 0)
            {
                CHECK(p->client.Send(m));
                expect_server += m;
            }
            else
            {
                CHECK(p->server.Send(m));
                expect_client += m;
            }
            if (i % 50 == 0)
                p->Run();
        }
        p->Run();
        CHECK(p->server.plain == expect_server && p->client.plain == expect_client);
    }
    {
        auto p = Pair(*sc, *cc);
        p->Run();
        CHECK(p->client.layer->Wrap({}) == LayerStatus::Ok); // 空写是空操作
        CHECK(p->client.TakeWire().empty());
    }
}

// 层契约：握手完成与明文可以在同一次 Unwrap 里到达（TLS 1.3 客户端发完 Finished 立刻发数据）：
// 明文照常交付，HandshakeDone 随后才报（连接把"交付明文"当作握手完成的信号）。
void TestHandshakeCompletesInTheSameUnwrapAsPlaintext()
{
    const auto sc = MustServer(ServerOpts(P().server));
    const auto cc = MustClient(ClientOpts(P().ca));
    auto p = Pair(*sc, *cc);
    p->client.Start();
    p->server.Feed(p->client.TakeWire());   // ClientHello
    p->client.Feed(p->server.TakeWire());   // ServerHello … Finished：客户端握手完成
    CHECK(p->client.handshake_done_count == 1 && p->server.handshake_done_count == 0);
    CHECK(p->client.Send("early-data"));     // 客户端的 Finished 与第一条数据攒在同一批
    p->server.Feed(p->client.TakeWire());    // 服务端一次 Unwrap：先完成握手、再读出数据
    CHECK(p->server.plain == "early-data");
    CHECK(p->server.plain_seen_before_done); // 明文先于 HandshakeDone 交付
    CHECK(p->server.handshake_done_count == 1);
}

// ---- 关闭与错误 ----

void TestCloseNotify()
{
    const auto sc = MustServer(ServerOpts(P().server));
    const auto cc = MustClient(ClientOpts(P().ca));
    auto p = Pair(*sc, *cc);
    p->Run();
    CHECK(p->client.layer->Shutdown() == LayerStatus::Ok);
    p->Run();
    CHECK(p->server.peer_shutdown && !p->server.errored);
    CHECK(p->client.layer->Shutdown() == LayerStatus::Ok); // 幂等：不再产生字节
    CHECK(p->client.TakeWire().empty());
}

void TestGarbageAndPlaintextAreRejected()
{
    const auto sc = MustServer(ServerOpts(P().server));
    const auto cc = MustClient(ClientOpts(P().ca));
    {
        // 明文 HTTP 打到 TLS 服务端
        LayerEnd server;
        server.layer = sc->Create(LayerRole::Server);
        const std::string http = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
        server.Feed(std::as_bytes(std::span(http)));
        CHECK(server.errored && server.layer->error_code() != 0 && server.handshake_done_count == 0);
    }
    {
        // 明文 HTTP 应答打到 TLS 客户端
        LayerEnd client;
        client.layer = cc->Create(LayerRole::Client);
        client.Start();
        const std::string http = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
        client.Feed(std::as_bytes(std::span(http)));
        CHECK(client.errored && client.handshake_done_count == 0);
    }
    {
        Rng rng{99};
        for (int i = 0; i < 50; ++i)
        {
            LayerEnd server;
            server.layer = sc->Create(LayerRole::Server);
            std::string junk(1 + rng.Below(300), '\0');
            for (auto& c : junk)
                c = static_cast<char>(rng());
            server.Feed(std::as_bytes(std::span(junk)), 1 + rng.Below(50));
            CHECK(server.handshake_done_count == 0); // 不崩，也不会"握手完成"
        }
    }
}

void TestTamperedRecordIsRejected()
{
    const auto sc = MustServer(ServerOpts(P().server));
    const auto cc = MustClient(ClientOpts(P().ca));
    for (const std::size_t flip_at : {std::size_t{0}, std::size_t{4}, std::size_t{12}, std::size_t{20}})
    {
        auto p = Pair(*sc, *cc);
        p->Run();
        CHECK(p->client.Send("sensitive payload that must not be altered"));
        auto wire = p->client.TakeWire();
        CHECK(wire.size() > flip_at);
        wire[flip_at] ^= std::byte{0x01};
        p->server.Feed(wire);
        CHECK(p->server.errored && p->server.plain.empty());
    }
}

// ---- 证书校验 ----

void TestCertificateVerification()
{
    const auto sc = MustServer(ServerOpts(P().server));
    {
        // 备用主机名也在 SAN 里
        const auto cc = MustClient(ClientOpts(P().ca, "alt.test"));
        auto p = Pair(*sc, *cc);
        p->Run();
        ExpectHandshakeOk(*p);
    }
    {
        const auto cc = MustClient(ClientOpts(P().ca, "other.test")); // 主机名不对
        auto p = Pair(*sc, *cc);
        p->Run();
        ExpectHandshakeFails(*p);
        CHECK(p->client.errored);
    }
    {
        const auto cc = MustClient(ClientOpts(P().other_ca)); // 不信任签发者
        auto p = Pair(*sc, *cc);
        p->Run();
        ExpectHandshakeFails(*p);
        CHECK(p->client.errored);
    }
    {
        const auto cc = MustClient(ClientOpts(P().ca, "127.0.0.1")); // IP 字面量按 IP SAN 校验
        auto p = Pair(*sc, *cc);
        p->Run();
        ExpectHandshakeOk(*p);
    }
    {
        const auto cc = MustClient(ClientOpts(P().ca, "[::1]")); // 带方括号的 IPv6
        auto p = Pair(*sc, *cc);
        p->Run();
        ExpectHandshakeOk(*p);
    }
    {
        const auto cc = MustClient(ClientOpts(P().ca, "127.0.0.2")); // IP 不在 SAN 里
        auto p = Pair(*sc, *cc);
        p->Run();
        ExpectHandshakeFails(*p);
    }
    {
        // 过期的服务端证书
        const TestCert expired = MakeCert({.cn = "server.test", .dns = {"server.test"}, .issuer = &P().ca,
                                           .not_before_sec = -7200, .not_after_sec = -3600});
        const auto esc = MustServer(ServerOpts(expired));
        const auto cc = MustClient(ClientOpts(P().ca));
        auto p = Pair(*esc, *cc);
        p->Run();
        ExpectHandshakeFails(*p);
        CHECK(p->client.errored);
    }
    {
        // 还没生效的证书
        const TestCert future = MakeCert({.cn = "server.test", .dns = {"server.test"}, .issuer = &P().ca,
                                          .not_before_sec = 3600, .not_after_sec = 7200});
        const auto fsc = MustServer(ServerOpts(future));
        const auto cc = MustClient(ClientOpts(P().ca));
        auto p = Pair(*fsc, *cc);
        p->Run();
        ExpectHandshakeFails(*p);
    }
    {
        // verify_peer = false：自签、名字对不上也能连（只给测试 / 内网用）
        const TestCert self = MakeCert({.cn = "whatever"});
        const auto ssc = MustServer(ServerOpts(self));
        TlsClientOptions co;
        co.verify_peer = false;
        const auto cc = MustClient(co);
        auto p = Pair(*ssc, *cc);
        p->Run();
        ExpectHandshakeOk(*p);
    }
    {
        // RSA 证书同样可用
        const TestCert rsa = MakeCert({.cn = "server.test", .dns = {"server.test"}, .issuer = &P().ca, .rsa = true});
        const auto rsc = MustServer(ServerOpts(rsa));
        const auto cc = MustClient(ClientOpts(P().ca));
        auto p = Pair(*rsc, *cc);
        p->Run();
        ExpectHandshakeOk(*p);
    }
    {
        // 带中间证书：叶证书 + 中间 CA 拼成链，客户端只信任根
        const TestCert inter = MakeCert({.cn = "Intermediate", .is_ca = true, .issuer = &P().ca});
        const TestCert leaf = MakeCert({.cn = "server.test", .dns = {"server.test"}, .issuer = &inter});
        TlsServerOptions so;
        so.cert_chain.pem = leaf.cert_pem + inter.cert_pem;
        so.private_key.pem = leaf.key_pem;
        const auto csc = MustServer(so);
        const auto cc = MustClient(ClientOpts(P().ca));
        auto p = Pair(*csc, *cc);
        p->Run();
        ExpectHandshakeOk(*p);
    }
}

// ---- mTLS ----

void TestMutualTls()
{
    const auto cc_with_cert = [&]
    {
        auto co = ClientOpts(P().ca);
        co.cert_chain.pem = P().client.cert_pem;
        co.private_key.pem = P().client.key_pem;
        return MustClient(co);
    }();
    const auto cc_without = MustClient(ClientOpts(P().ca));

    auto require = ServerOpts(P().server);
    require.client_ca.pem = P().ca.cert_pem;
    require.require_client_cert = true;
    const auto sc_require = MustServer(require);
    {
        auto p = Pair(*sc_require, *cc_with_cert);
        p->Run();
        ExpectHandshakeOk(*p);
        CHECK(L(p->server).peer_subject().find("client-1") != std::string::npos);
    }
    {
        auto p = Pair(*sc_require, *cc_without); // 要求但没带
        p->Run();
        ExpectServerRejectsClient(*p);
    }
    {
        // 带了，但不是信任的 CA 签的
        const TestCert rogue = MakeCert({.cn = "rogue", .issuer = &P().other_ca});
        auto co = ClientOpts(P().ca);
        co.cert_chain.pem = rogue.cert_pem;
        co.private_key.pem = rogue.key_pem;
        const auto cc = MustClient(co);
        auto p = Pair(*sc_require, *cc);
        p->Run();
        ExpectServerRejectsClient(*p);
    }

    auto optional = ServerOpts(P().server);
    optional.client_ca.pem = P().ca.cert_pem;
    const auto sc_optional = MustServer(optional); // 要校验，但不强制
    {
        auto p = Pair(*sc_optional, *cc_without);
        p->Run();
        ExpectHandshakeOk(*p);
        CHECK(L(p->server).peer_subject().empty());
    }
    {
        auto p = Pair(*sc_optional, *cc_with_cert);
        p->Run();
        ExpectHandshakeOk(*p);
        CHECK(!L(p->server).peer_subject().empty());
    }
}

// ---- 版本 ----

void TestVersionNegotiation()
{
    auto so = ServerOpts(P().server);
    so.min_version = TlsVersion::Tls13;
    const auto sc13 = MustServer(so);
    auto co12 = ClientOpts(P().ca);
    co12.max_version = TlsVersion::Tls12;
    const auto cc12 = MustClient(co12);
    {
        auto p = Pair(*sc13, *cc12); // 服务端只收 1.3、客户端最高 1.2
        p->Run();
        ExpectHandshakeFails(*p);
    }
    const auto cc13 = MustClient(ClientOpts(P().ca));
    {
        auto p = Pair(*sc13, *cc13);
        p->Run();
        ExpectHandshakeOk(*p);
        CHECK(L(p->client).protocol_version() == 0x0304);
    }
}

// ---- session ticket 恢复 ----

void TestSessionResumption()
{
    for (const bool tls12 : {false, true})
    {
        {
            // 服务端不存会话（SSL_SESS_CACHE_OFF），靠无状态 ticket 恢复
            auto so = ServerOpts(P().server);
            auto co = ClientOpts(P().ca);
            if (tls12)
                so.max_version = co.max_version = TlsVersion::Tls12;
            const auto sc = MustServer(so);
            const auto cc = MustClient(co);
            auto first = Pair(*sc, *cc);
            // 注意：这里**没有**任何应用数据往返。BoringSSL 的 TLS 1.3 服务端把 NewSessionTicket 推迟到第一次写，
            // 所以服务端层在握手完成时要做一次零字节写把它刷出来（TlsLayer::Unwrap）；没有那一步，下面每次重连都是完整握手。
            first->Run(); // 泵到没有字节流动：TLS 1.3 的 NewSessionTicket 也被客户端收下了
            ExpectHandshakeOk(*first);
            CHECK(!L(first->client).session_reused() && !L(first->server).session_reused());
            for (int i = 0; i < 5; ++i)
            {
                auto again = Pair(*sc, *cc);
                again->Run();
                ExpectHandshakeOk(*again);
                CHECK(L(again->client).session_reused() && L(again->server).session_reused());
                CHECK(again->client.Send("after resumption"));
                again->Run();
                CHECK(again->server.plain == "after resumption");
            }
        }
        {
            // 服务端关掉 ticket：不能恢复
            auto so = ServerOpts(P().server);
            so.session_tickets = false;
            auto co = ClientOpts(P().ca);
            if (tls12)
                so.max_version = co.max_version = TlsVersion::Tls12;
            const auto sc = MustServer(so);
            const auto cc = MustClient(co);
            auto first = Pair(*sc, *cc);
            first->Run();
            auto again = Pair(*sc, *cc);
            again->Run();
            ExpectHandshakeOk(*again);
            CHECK(!L(again->client).session_reused());
        }
        {
            // 客户端关掉恢复
            auto so = ServerOpts(P().server);
            auto co = ClientOpts(P().ca);
            co.session_resumption = false;
            if (tls12)
                so.max_version = co.max_version = TlsVersion::Tls12;
            const auto sc = MustServer(so);
            const auto cc = MustClient(co);
            auto first = Pair(*sc, *cc);
            first->Run();
            auto again = Pair(*sc, *cc);
            again->Run();
            ExpectHandshakeOk(*again);
            CHECK(!L(again->client).session_reused());
        }
    }
}

// ---- 线程本地的错误队列 ----

// BoringSSL 的错误队列是线程本地的：一条连接的握手错误如果没被清掉，会污染同一线程上下一条连接的 SSL_get_error。
void TestErrorQueueDoesNotLeakBetweenConnections()
{
    const auto sc = MustServer(ServerOpts(P().server));
    const auto bad_client = MustClient(ClientOpts(P().ca, "wrong.name"));
    const auto good_client = MustClient(ClientOpts(P().ca));
    for (int i = 0; i < 20; ++i)
    {
        auto bad = Pair(*sc, *bad_client);
        auto good = Pair(*sc, *good_client);
        // 交替推进，让坏连接的错误留在队列里时好连接正在握手
        for (int round = 0; round < 50; ++round)
        {
            const bool b = bad->Pump();
            const bool g = good->Pump();
            if (!b && !g)
                break;
        }
        ExpectHandshakeFails(*bad);
        ExpectHandshakeOk(*good);
        CHECK(good->client.Send("ok") && !good->client.errored);
        good->Run();
        CHECK(good->server.plain == "ok");
    }
}

// 别的连接在这个线程上留下的陈旧错误，不能让下一次调用误判：Unwrap 进 SSL 之前必须先清错误队列，否则一次本来只是"数据还没到齐"
// （WANT_READ）的返回会被 SSL_get_error 读成 SSL_ERROR_SSL。TestErrorQueueDoesNotLeakBetweenConnections 是整体场景，这里直接塞一个
// 陈旧错误再调。
void TestStaleThreadErrorDoesNotBreakUnwrap()
{
    const auto sc = MustServer(ServerOpts(P().server));
    const auto cc = MustClient(ClientOpts(P().ca));
    LayerEnd client;
    client.layer = cc->Create(LayerRole::Client);
    client.Start();
    const std::vector<std::byte> hello = client.TakeWire();
    CHECK(hello.size() > 20);

    LayerEnd server;
    server.layer = sc->Create(LayerRole::Server);
    // 只给半个 ClientHello（合法，只是还没到齐）：应当返回 Ok，等下一批
    ERR_put_error(ERR_LIB_SSL, 0, 1, __FILE__, __LINE__); // 别的连接留下的陈旧错误
    std::vector<std::byte> head(hello.begin(), hello.begin() + 10);
    CHECK(server.layer->Unwrap(head, [](std::span<std::byte>) { return true; }) == LayerStatus::Ok);
    CHECK(!server.errored);
    // 余下的到齐：握手继续
    ERR_put_error(ERR_LIB_SSL, 0, 1, __FILE__, __LINE__);
    std::vector<std::byte> rest(hello.begin() + 10, hello.end());
    server.Feed(rest);
    CHECK(!server.errored && !server.TakeWire().empty()); // 服务端产出了 ServerHello …
}

// 契约违例要大声报错，而不是悄悄丢数据：握手完成之前连接不会调 Wrap，万一调了（SSL_write 此时返回 WANT_READ）就是 Error，
// 并且错误码非 0。
void TestWrapBeforeHandshakeIsAnError()
{
    const auto cc = MustClient(ClientOpts(P().ca));
    LayerEnd client;
    client.layer = cc->Create(LayerRole::Client);
    client.Start();
    const std::string data = "premature";
    CHECK(client.layer->Wrap(std::as_bytes(std::span(data))) == LayerStatus::Error);
    CHECK(client.layer->error_code() != 0);
}

// SNI 是明文出现在 ClientHello 里的：域名要带上；IP 字面量不发（RFC 6066 不允许 IP 作 server_name）。
void TestClientHelloCarriesSniOnlyForHostnames()
{
    const auto wire_of = [](const TlsClientOptions& o)
    {
        const auto cc = MustClient(o);
        LayerEnd client;
        client.layer = cc->Create(LayerRole::Client);
        client.Start();
        const auto w = client.TakeWire();
        return std::string(reinterpret_cast<const char*>(w.data()), w.size());
    };
    CHECK(wire_of(ClientOpts(P().ca, "sni-check.example.com")).find("sni-check.example.com") != std::string::npos);
    CHECK(wire_of(ClientOpts(P().ca, "127.0.0.1")).find("127.0.0.1") == std::string::npos);
    CHECK(wire_of(ClientOpts(P().ca, "[::1]")).find("::1") == std::string::npos);
}

// sink 返回 false（连接在回调里被关了）：层停止交付，不崩。
void TestSinkStopsDelivery()
{
    const auto sc = MustServer(ServerOpts(P().server));
    const auto cc = MustClient(ClientOpts(P().ca));
    auto p = Pair(*sc, *cc);
    p->Run();
    for (int i = 0; i < 5; ++i)
        CHECK(p->client.Send("chunk-" + std::to_string(i)));
    auto wire = p->client.TakeWire();
    int delivered = 0;
    const LayerStatus st = p->server.layer->Unwrap(wire,
                                                   [&](std::span<std::byte>)
                                                   {
                                                       ++delivered;
                                                       return false;
                                                   });
    CHECK(st == LayerStatus::Ok && delivered == 1);
}

// ---- 上下文的创建 ----

void TestContextCreationErrors()
{
    std::string err;
    {
        TlsServerOptions o; // 什么都没配
        CHECK(TlsContext::CreateServer(o, err) == nullptr && !err.empty());
    }
    {
        TlsServerOptions o;
        o.cert_chain.file = "/nonexistent/cert.pem";
        o.private_key.file = "/nonexistent/key.pem";
        CHECK(TlsContext::CreateServer(o, err) == nullptr && err.find("/nonexistent/cert.pem") != std::string::npos);
    }
    {
        TlsServerOptions o = ServerOpts(P().server);
        o.private_key.pem.clear(); // 只给了证书
        CHECK(TlsContext::CreateServer(o, err) == nullptr && !err.empty());
    }
    {
        TlsServerOptions o = ServerOpts(P().server);
        o.private_key.pem = P().client.key_pem; // 证书与私钥不配对
        CHECK(TlsContext::CreateServer(o, err) == nullptr && !err.empty());
    }
    {
        TlsServerOptions o = ServerOpts(P().server);
        o.cert_chain.pem = "-----BEGIN CERTIFICATE-----\nnot base64 at all\n-----END CERTIFICATE-----\n";
        CHECK(TlsContext::CreateServer(o, err) == nullptr && !err.empty());
    }
    {
        TlsServerOptions o = ServerOpts(P().server);
        o.require_client_cert = true; // 没有 client_ca
        CHECK(TlsContext::CreateServer(o, err) == nullptr && err.find("client_ca") != std::string::npos);
    }
    {
        TlsServerOptions o = ServerOpts(P().server);
        o.client_ca.pem = "garbage";
        CHECK(TlsContext::CreateServer(o, err) == nullptr && !err.empty());
    }
    {
        TlsClientOptions o; // verify_peer 为真但没有 server_name
        CHECK(TlsContext::CreateClient(o, err) == nullptr && err.find("server_name") != std::string::npos);
    }
    {
        TlsClientOptions o = ClientOpts(P().ca);
        o.ca.pem = "garbage";
        CHECK(TlsContext::CreateClient(o, err) == nullptr && !err.empty());
    }
    {
        TlsClientOptions o = ClientOpts(P().ca);
        o.cert_chain.pem = P().client.cert_pem; // 客户端证书没配私钥
        CHECK(TlsContext::CreateClient(o, err) == nullptr && !err.empty());
    }
    {
        TlsClientOptions o = ClientOpts(P().ca);
        CHECK(TlsContext::CreateClient(o, err) != nullptr && err.empty()); // 成功时 error 被清空
    }
}

// 证书与私钥从文件读。
void TestContextFromFiles()
{
    char dir[] = "/tmp/breeze-tls-XXXXXX";
    CHECK(::mkdtemp(dir) != nullptr);
    const std::string d = dir;
    const auto write = [&](const std::string& name, const std::string& text)
    {
        std::FILE* f = std::fopen((d + "/" + name).c_str(), "wb");
        CHECK(f != nullptr);
        CHECK(std::fwrite(text.data(), 1, text.size(), f) == text.size());
        std::fclose(f);
    };
    write("server.pem", P().server.cert_pem);
    write("server.key", P().server.key_pem);
    write("ca.pem", P().ca.cert_pem);
    TlsServerOptions so;
    so.cert_chain.file = d + "/server.pem";
    so.private_key.file = d + "/server.key";
    TlsClientOptions co;
    co.server_name = "server.test";
    co.ca.file = d + "/ca.pem";
    {
        const auto sc = MustServer(so);
        const auto cc = MustClient(co);
        auto p = Pair(*sc, *cc);
        p->Run();
        ExpectHandshakeOk(*p);
    }
    for (const char* n : {"server.pem", "server.key", "ca.pem"})
        ::unlink((d + "/" + n).c_str());
    ::rmdir(d.c_str());
}

void TestHolderReload()
{
    const TestCert one = MakeCert({.cn = "one", .dns = {"server.test"}, .issuer = &P().ca});
    const TestCert two = MakeCert({.cn = "two", .dns = {"server.test"}, .issuer = &P().ca});
    TlsContextHolder holder(MustServer(ServerOpts(one)));
    const auto cc = MustClient(ClientOpts(P().ca));

    auto old_conn = Pair(holder, *cc);
    old_conn->Run();
    ExpectHandshakeOk(*old_conn);
    CHECK(L(old_conn->client).peer_subject().find("CN=one") != std::string::npos);

    holder.Reload(MustServer(ServerOpts(two)));
    auto new_conn = Pair(holder, *cc);
    new_conn->Run();
    ExpectHandshakeOk(*new_conn);
    CHECK(L(new_conn->client).peer_subject().find("CN=two") != std::string::npos);

    // 已建立的连接不受 Reload 影响，仍能收发
    CHECK(old_conn->client.Send("still works"));
    old_conn->Run();
    CHECK(old_conn->server.plain == "still works");
}

// 上下文先于它创建的连接销毁（热更证书后旧上下文被释放）：连接各自持有 SSL_CTX 的引用，客户端的会话回调也不依赖 TlsContext。
void TestContextOutlivedByConnections()
{
    std::unique_ptr<LayerPair> p;
    {
        const auto sc = MustServer(ServerOpts(P().server));
        const auto cc = MustClient(ClientOpts(P().ca));
        p = Pair(*sc, *cc);
        p->Run();
        ExpectHandshakeOk(*p);
    } // 两个上下文在这里销毁
    CHECK(p->client.Send("after contexts are gone"));
    CHECK(p->server.Send("both ways"));
    p->Run(); // 收发之外，TLS 1.3 的后续握手消息（ticket）也会走会话回调
    CHECK(p->server.plain == "after contexts are gone" && p->client.plain == "both ways");
}

// 每条空闲的服务端 TLS 连接占多少堆内存（握手完成之后）：BoringSSL 的 SSL 状态 + 我们的 TlsLayer 与发送缓冲。
// 方案 §4.1 的 2.8KB 是 BoringSSL 自己的数字（开 shed_handshake_config），不含我们这一层；这里量全部。
// glibc 的 mallinfo2；消毒器下 malloc 被替换，数字没有意义，跳过。
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define BREEZE_TLS_NO_MEM_PROBE 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define BREEZE_TLS_NO_MEM_PROBE 1
#endif
#endif

void TestIdleServerLayerMemory()
{
#ifdef BREEZE_TLS_NO_MEM_PROBE
    return;
#else
    const auto sc = MustServer(ServerOpts(P().server));
    const auto cc = MustClient(ClientOpts(P().ca));
    constexpr int kN = 2000;
    std::vector<std::unique_ptr<IStreamLayer>> servers;
    servers.reserve(kN);
    {
        // 先预热一轮，让 BoringSSL 的一次性全局初始化（随机数、表）不计入
        auto warm = Pair(*sc, *cc);
        warm->Run();
    }
    const std::size_t before = ::mallinfo2().uordblks;
    for (int i = 0; i < kN; ++i)
    {
        auto p = Pair(*sc, *cc);
        p->Run();
        CHECK(p->server.handshake_done_count == 1);
        servers.push_back(std::move(p->server.layer)); // 只留服务端；客户端随 p 一起释放
    }
    const std::size_t after = ::mallinfo2().uordblks;
    const std::size_t per = (after - before) / kN;
    std::fprintf(stderr, "[内存] 握手完成后每条服务端 TLS 层占堆 %zu 字节（含 SSL 状态、TlsLayer、发送缓冲）\n", per);
    // 方案 §4.1：BoringSSL 自己约 2.8KB（开 shed）~ 3.9KB（不开）；加上我们这层实测约 3.2KB。上限卡在 3.8KB：
    // 忘了开 shed_handshake_config（≈ 4.3KB）、或服务端缓存了会话（每条再多约 1KB）都会超。升级 BoringSSL 后数字变了就重新量、改这里。
    CHECK(per < 3800);
#endif
}

// 协商出的 group 必须是 X25519：BoringSSL 的默认会选后量子混合的 X25519MLKEM768（服务端多约 40% CPU、握手包更大），
// 游戏流量没有"先抓包后解密"的价值，所以我们只开 X25519 + P-256。用一个**默认配置**的客户端（它会提供 MLKEM）验证服务端自己做了限制。
void TestGroupsAreRestrictedToX25519()
{
    for (const bool tls12 : {false, true})
    {
        auto so = ServerOpts(P().server);
        if (tls12)
            so.max_version = TlsVersion::Tls12;
        const auto sc = MustServer(so);
        SSL_CTX* raw = SSL_CTX_new(TLS_method()); // 默认配置，没有限制 groups
        SSL_CTX_set_verify(raw, SSL_VERIFY_NONE, nullptr);
        SSL* ssl = SSL_new(raw);
        SSL_CTX_free(raw); // SSL 自己持有引用
        LayerPair p(std::make_unique<tls::TlsLayer>(ssl, LayerRole::Client), sc->Create(LayerRole::Server));
        p.Run();
        ExpectHandshakeOk(p);
        CHECK(SSL_get_group_id(L(p.server).native()) == SSL_GROUP_X25519);
        CHECK(SSL_get_group_id(L(p.client).native()) == SSL_GROUP_X25519);
    }
    {
        // 我们自己的客户端同样只选 X25519
        const auto sc = MustServer(ServerOpts(P().server));
        const auto cc = MustClient(ClientOpts(P().ca));
        auto p = Pair(*sc, *cc);
        p->Run();
        CHECK(SSL_get_group_id(L(p->client).native()) == SSL_GROUP_X25519);
    }
}

} // namespace

int main()
{
    TestHandshakeAndDataBothDirections();
    TestChunkingInvariance();
    TestManyRecordsInOneUnwrap();
    TestLargePayloadAndManySmallMessages();
    TestHandshakeCompletesInTheSameUnwrapAsPlaintext();
    TestCloseNotify();
    TestGarbageAndPlaintextAreRejected();
    TestTamperedRecordIsRejected();
    TestCertificateVerification();
    TestMutualTls();
    TestVersionNegotiation();
    TestSessionResumption();
    TestErrorQueueDoesNotLeakBetweenConnections();
    TestStaleThreadErrorDoesNotBreakUnwrap();
    TestWrapBeforeHandshakeIsAnError();
    TestClientHelloCarriesSniOnlyForHostnames();
    TestSinkStopsDelivery();
    TestContextCreationErrors();
    TestContextFromFiles();
    TestHolderReload();
    TestContextOutlivedByConnections();
    TestIdleServerLayerMemory();
    TestGroupsAreRestrictedToX25519();
    std::puts("tls_layer_test: ok");
    return 0;
}
