// WebSocket codec（服务端 / 客户端）：用假宿主（FakeHost）直接喂字节，不开 socket。
// Peer 模拟连接的解码循环（半个帧留在缓冲里、Consumed 推进、Error 即停），并检查契约：NeedMore / Error 不得改动缓冲、
// Consumed 必须推进。帧的构造与解析用 testing/ws_test_util.h 里**独立手写**的那份（不复用被测代码的 ws_frame），做对照。
//
// 覆盖：握手（RFC 例子的逐字节应答、一字节一字节喂、紧跟的首批帧）、拒绝的状态码表、健康检查、头部上限、on_upgrade、
// 子协议；数据帧 / 分片 / Ping Pong / 关闭握手 / OnClosing；长度上限（帧头一到就判）、UTF-8；
// 任意切分得到相同结果（性质测试）、随机垃圾不崩；客户端的升级请求、应答校验、掩码；客户端 ↔ 服务端对跑。
// 无测试框架依赖：失败即 abort。

#include "base/net/ws/ws_codec.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/net/codec/codec_host_interface.h"
#include "base/net/testing/ws_test_util.h"
#include "base/net/ws/internal/http_upgrade.h"

namespace
{

using namespace breeze;
using namespace breeze::testing;
using Strs = std::vector<std::string>;


// ---- 假宿主与 Peer ----

struct Closed
{
    CloseReason reason;
    int code;
};

class FakeHost final : public ICodecHost
{
public:
    void SendRaw(std::span<const std::byte> b) override
    {
        sent.append(reinterpret_cast<const char*>(b.data()), b.size());
        events.push_back("S");
    }
    void HandshakeDone() override
    {
        ++handshake_done;
        events.push_back("H");
    }
    void CloseAfterFlush(CloseReason r, int c) override
    {
        ++close_calls;
        if (!closed)
            closed = Closed{r, c};
        events.push_back("X");
    }
    void SetContext(std::unique_ptr<ConnectionContext> c) override
    {
        ctx = std::move(c);
        events.push_back("C");
    }
    const PeerInfo& peer() const noexcept override { return peer_; }

    std::string sent; // SendRaw 写出的全部字节
    Strs events;      // S = 一次 SendRaw、H = HandshakeDone、X = CloseAfterFlush、C = SetContext、M = 一条消息
    int handshake_done = 0;
    int close_calls = 0;
    std::optional<Closed> closed;
    std::unique_ptr<ConnectionContext> ctx;
    PeerInfo peer_;
};

struct Peer
{
    explicit Peer(const IFrameCodecFactory& f, std::optional<Endpoint> ep = std::nullopt)
    {
        host.peer_.endpoint = std::move(ep);
        codec = f.Create();
        codec->Attach(host);
    }

    void Feed(std::string_view s)
    {
        if (error)
            return;
        const auto* p = reinterpret_cast<const std::byte*>(s.data());
        buf.insert(buf.end(), p, p + s.size());
        std::size_t total = 0;
        for (int guard = 0; total < buf.size(); ++guard)
        {
            CHECK(guard < 1000000); // 不能原地空转
            const std::span<std::byte> view = std::span(buf).subspan(total);
            const std::vector<std::byte> snapshot = view.size() <= 8192 ? std::vector<std::byte>(view.begin(), view.end())
                                                                        : std::vector<std::byte>();
            std::size_t used = 0xDEAD;
            const DecodeResult r = codec->Decode(
                view,
                [this](std::span<const std::byte> payload)
                {
                    messages.emplace_back(reinterpret_cast<const char*>(payload.data()), payload.size());
                    host.events.push_back("M");
                },
                used);
            if (r == DecodeResult::Ok || r == DecodeResult::Consumed)
            {
                CHECK(used > 0 && used <= view.size()); // Consumed 必须推进
                total += used;
                continue;
            }
            // NeedMore / Error：缓冲不得被改动（同一批字节还会再传入）
            if (!snapshot.empty())
                CHECK(std::equal(snapshot.begin(), snapshot.end(), view.begin()));
            if (r == DecodeResult::Error)
                error = true;
            else
                CHECK(used == 0);
            break;
        }
        buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(total));
    }

    FakeHost host;
    std::unique_ptr<IFrameCodec> codec;
    std::vector<std::byte> buf; // 还没解完的字节
    Strs messages;
    bool error = false; // Decode 返回过 Error
};

std::unique_ptr<Peer> OpenServer(WsServerOptions o = {})
{
    const WsServerCodecFactory f(std::move(o));
    auto p = std::make_unique<Peer>(f);
    p->Feed(WsHandshakeRequest());
    CHECK(p->host.handshake_done == 1 && !p->error);
    p->host.sent.clear();
    p->host.events.clear();
    return p;
}

// 服务端关闭了（发 Close 帧、CloseAfterFlush）：检查 Close 帧与关闭原因，并验证此后的字节一律被丢弃。
void ExpectServerFailed(Peer& p, int code)
{
    const auto frames = WsParseFrames(p.host.sent, false);
    CHECK(frames.size() == 1 && frames[0].opcode == 8 && frames[0].fin && frames[0].payload == WsCloseBody(code));
    CHECK(p.host.closed && p.host.closed->reason == CloseReason::ProtocolError && p.host.closed->code == code);
    CHECK(p.host.close_calls == 1);
    const std::string before = p.host.sent;
    const std::size_t msgs = p.messages.size();
    p.Feed(WsFrame(2, "ignored"));
    p.Feed(WsFrame(8, WsCloseBody(1000)));
    CHECK(p.host.sent == before && p.messages.size() == msgs && p.host.close_calls == 1 && !p.error);
}

// ---- 服务端：握手 ----

void TestServerHandshakeExactResponse()
{
    const WsServerCodecFactory f;
    Peer p(f);
    CHECK(p.codec->traits().needs_handshake && !p.codec->traits().encodes_payload);
    CHECK(p.host.handshake_done == 0 && p.host.sent.empty());
    p.Feed(WsHandshakeRequest());
    CHECK(p.host.sent == kWs101);
    CHECK((p.host.events == Strs{"S", "H"})); // 应答先于握手完成（OnConnected 之前客户端就能看到 101）
    CHECK(!p.host.closed && !p.error && p.buf.empty());
}

void TestServerHandshakeByteByByte()
{
    const WsServerCodecFactory f;
    Peer p(f);
    const std::string req = WsHandshakeRequest();
    for (std::size_t i = 0; i < req.size(); ++i)
    {
        CHECK(p.host.handshake_done == 0 && p.host.sent.empty());
        p.Feed(req.substr(i, 1));
    }
    CHECK(p.host.handshake_done == 1 && p.host.sent == kWs101 && !p.error);
}

void TestServerPipelinedFrames()
{
    const WsServerCodecFactory f;
    {
        Peer p(f);
        p.Feed(WsHandshakeRequest() + WsFrame(2, "first") + WsFrame(2, "second"));
        CHECK((p.host.events == Strs{"S", "H", "M", "M"})); // 握手完成先于消息
        CHECK((p.messages == Strs{"first", "second"}));
    }
    {
        // 第一个帧被切在两次到达之间
        Peer p(f);
        const std::string fr = WsFrame(2, "split-me");
        p.Feed(WsHandshakeRequest() + fr.substr(0, 5));
        CHECK(p.host.handshake_done == 1 && p.messages.empty());
        p.Feed(fr.substr(5));
        CHECK((p.messages == Strs{"split-me"}));
    }
}

void TestServerRejects()
{
    struct Case
    {
        std::string request;
        int status;
        std::string path_option;
    };
    std::string many = "GET /ws HTTP/1.1\r\nHost: h\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " +
                       std::string(kWsKey) + "\r\nSec-WebSocket-Version: 13\r\n";
    for (int i = 0; i < 70; ++i)
        many += "X" + std::to_string(i) + ": v\r\n";
    many += "\r\n";

    const std::vector<Case> cases = {
        {"POST /ws HTTP/1.1\r\nHost: h\r\n\r\n", 405, ""},
        {"GET /ws HTTP/1.0\r\nHost: h\r\n\r\n", 400, ""},
        {"GARBAGE\r\n\r\n", 400, ""},
        {WsHandshakeRequest("/ws", "", "short"), 400, ""},
        {"GET /ws HTTP/1.1\r\nHost: h\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\n\r\n", 400, ""},
        {WsHandshakeRequest("/ws", "Content-Length: 5\r\n"), 400, ""},
        {WsHandshakeRequest("/ws", "X: a\r\n b\r\n"), 400, ""}, // obs-fold
        {WsHandshakeRequest("/ws", "Origin: https://evil.example\r\n"), 403, ""},
        {WsHandshakeRequest("/other"), 404, "/game"},
        {"GET /ws HTTP/1.1\r\nHost: h\r\n\r\n", 426, ""},
        {"GET /ws HTTP/1.1\r\nHost: h\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + std::string(kWsKey) +
             "\r\nSec-WebSocket-Version: 8\r\n\r\n",
         426, ""},
        {many, 431, ""},
    };
    for (const Case& c : cases)
    {
        WsServerOptions o;
        o.path = c.path_option;
        if (c.status == 403)
            o.allowed_origins = {"https://game.example"};
        const WsServerCodecFactory f(o);
        Peer p(f);
        p.Feed(c.request);
        CHECK(p.host.handshake_done == 0);
        CHECK(p.host.sent.starts_with("HTTP/1.1 " + std::to_string(c.status) + " "));
        CHECK(p.host.closed && p.host.closed->reason == CloseReason::ProtocolError && p.host.closed->code == c.status);
        CHECK((p.host.events == Strs{"S", "X"}));
        if (c.status == 426)
            CHECK(p.host.sent.find("Sec-WebSocket-Version: 13") != std::string::npos);
        // 拒绝之后再来的字节一律丢弃，不产生任何输出
        const std::string before = p.host.sent;
        p.Feed(WsFrame(2, "x"));
        p.Feed(WsHandshakeRequest());
        CHECK(p.host.sent == before && p.host.close_calls == 1 && p.messages.empty() && !p.error);
    }
}

void TestServerHealth()
{
    WsServerOptions o;
    o.health_path = "/healthz";
    const WsServerCodecFactory f(o);
    Peer p(f);
    p.Feed("GET /healthz HTTP/1.1\r\nHost: lb\r\n\r\n");
    CHECK(p.host.sent.starts_with("HTTP/1.1 200 OK\r\n") && p.host.sent.ends_with("\r\n\r\nok"));
    CHECK(p.host.closed && p.host.closed->reason == CloseReason::LocalClose && p.host.closed->code == 0); // 不算握手失败
    CHECK(p.host.handshake_done == 0);
}

void TestServerHeaderLimits()
{
    WsServerOptions o;
    o.max_header_bytes = 256;
    const std::size_t base = WsHandshakeRequest().size();
    CHECK(base + 9 < 256);
    const auto padded = [&](std::size_t total)
    {
        const std::size_t n = total - base - 9; // "X-Pad: " + n + "\r\n" = n + 9
        return WsHandshakeRequest("/ws", "X-Pad: " + std::string(n, 'a') + "\r\n");
    };
    {
        const WsServerCodecFactory f(o);
        Peer p(f);
        const std::string r = padded(256); // 恰好等于上限：接受
        CHECK(r.size() == 256);
        p.Feed(r);
        CHECK(p.host.handshake_done == 1 && p.host.sent == kWs101);
    }
    {
        const WsServerCodecFactory f(o);
        Peer p(f);
        const std::string r = padded(257); // 多一个字节：431
        CHECK(r.size() == 257);
        p.Feed(r);
        CHECK(p.host.handshake_done == 0 && p.host.sent.starts_with("HTTP/1.1 431 "));
    }
    {
        // 一直没有结束符：没到上限前只等，到了上限就拒绝
        const WsServerCodecFactory f(o);
        Peer p(f);
        p.Feed(std::string(255, 'a'));
        CHECK(p.host.sent.empty() && !p.host.closed);
        p.Feed("a");
        CHECK(p.host.sent.starts_with("HTTP/1.1 431 ") && p.host.closed);
    }
    {
        // 一次到达一大坨：头在上限之内结束，其余是紧跟的帧
        const WsServerCodecFactory f(o);
        Peer p(f);
        p.Feed(padded(256) + WsFrame(2, std::string(1000, 'z')));
        CHECK(p.host.handshake_done == 1 && p.messages.size() == 1 && p.messages[0].size() == 1000);
    }
}

struct SeenUpgrade
{
    std::string path, query, host, origin, token, subs;
    std::size_t header_count = 0;
    bool has_peer = false;
    std::string peer_addr;
    int calls = 0;
};

struct TestContext final : ConnectionContext
{
    explicit TestContext(std::string p)
        : path(std::move(p))
    {
    }
    std::string path;
};

void TestServerOnUpgrade()
{
    SeenUpgrade seen;
    WsServerOptions o;
    o.on_upgrade = [&seen](const WsUpgradeRequest& r, WsUpgradeReply& reply) noexcept
    {
        ++seen.calls;
        seen.path = std::string(r.path);
        seen.query = std::string(r.query);
        seen.host = std::string(r.host);
        seen.origin = std::string(r.origin);
        seen.token = std::string(r.Header("x-token"));
        for (const auto s : r.subprotocols)
            seen.subs += std::string(s) + ";";
        seen.header_count = r.headers.size();
        seen.has_peer = r.peer != nullptr && r.peer->endpoint.has_value();
        if (seen.has_peer)
            seen.peer_addr = r.peer->endpoint->address();
        reply.context = std::make_unique<TestContext>(std::string(r.path));
    };
    const WsServerCodecFactory f(o);
    Peer p(f, Endpoint::Tcp("10.1.2.3", 5555));
    p.Feed(WsHandshakeRequest("/ws?token=abc", "X-Token: t1\r\nOrigin: https://o\r\nSec-WebSocket-Protocol: a, b\r\n"));
    CHECK(seen.calls == 1 && seen.path == "/ws" && seen.query == "token=abc" && seen.host == "example.com");
    CHECK(seen.origin == "https://o" && seen.token == "t1" && seen.subs == "a;b;" && seen.header_count == 8);
    CHECK(seen.has_peer && seen.peer_addr == "10.1.2.3");
    CHECK(p.host.handshake_done == 1);
    CHECK((p.host.events == Strs{"S", "C", "H"})); // 先写应答，再装 context，最后完成握手（OnConnected 能看到 context）
    const auto* ctx = dynamic_cast<TestContext*>(p.host.ctx.get());
    CHECK(ctx != nullptr && ctx->path == "/ws");
}

void TestServerOnUpgradeRejects()
{
    for (const auto& [status, expect] : std::vector<std::pair<int, int>>{{403, 403}, {429, 429}, {503, 503}, {200, 500}, {399, 500}, {600, 500}})
    {
        WsServerOptions o;
        o.on_upgrade = [status = status](const WsUpgradeRequest&, WsUpgradeReply& reply) noexcept
        {
            reply.reject_status = status;
            reply.context = std::make_unique<TestContext>("x"); // 拒绝时的 context 要被丢掉
        };
        const WsServerCodecFactory f(o);
        Peer p(f);
        p.Feed(WsHandshakeRequest());
        CHECK(p.host.handshake_done == 0 && p.host.ctx == nullptr);
        CHECK(p.host.sent.starts_with("HTTP/1.1 " + std::to_string(expect) + " "));
        CHECK(p.host.closed && p.host.closed->code == expect);
    }
}

void TestServerSubprotocol()
{
    WsServerOptions o;
    o.subprotocols = {"v2", "v1"};
    {
        const WsServerCodecFactory f(o);
        Peer p(f);
        p.Feed(WsHandshakeRequest("/ws", "Sec-WebSocket-Protocol: v1, v2\r\n"));
        CHECK(p.host.sent.find("Sec-WebSocket-Protocol: v2\r\n") != std::string::npos); // 按服务端偏好
    }
    {
        const WsServerCodecFactory f(o);
        Peer p(f);
        p.Feed(WsHandshakeRequest("/ws", "Sec-WebSocket-Protocol: v9\r\n"));
        CHECK(p.host.handshake_done == 1 && p.host.sent.find("Sec-WebSocket-Protocol") == std::string::npos);
    }
    {
        const WsServerCodecFactory f; // 服务端没配子协议
        Peer p(f);
        p.Feed(WsHandshakeRequest("/ws", "Sec-WebSocket-Protocol: v1\r\n"));
        CHECK(p.host.handshake_done == 1 && p.host.sent == kWs101);
    }
}

// ---- 服务端：帧 ----

void TestServerDataFrames()
{
    auto p = OpenServer();
    p->Feed(WsFrame(2, "hello"));
    p->Feed(WsFrame(2, ""));
    p->Feed(WsFrame(2, std::string(300, 'b')));          // 4 字节头
    p->Feed(WsFrame(2, std::string(70000, 'c'), true, true, 0xA1B2C3D4)); // 10 字节头（默认上限 64KB，下面单独调大）
    CHECK(p->messages.size() == 3 && p->messages[0] == "hello" && p->messages[1].empty() && p->messages[2].size() == 300);
    CHECK(p->host.closed && p->host.closed->code == 1009); // 70000 > 默认 64KB
}

void TestServerFrameViolations()
{
    {
        auto p = OpenServer();
        p->Feed(WsFrame(2, "x", true, /*mask=*/false)); // 客户端没加掩码
        ExpectServerFailed(*p, 1002);
    }
    {
        auto p = OpenServer();
        p->Feed(WsFrame(2, "x").replace(0, 1, std::string(1, static_cast<char>(0xC2)))); // RSV1
        ExpectServerFailed(*p, 1002);
    }
    for (const int op : {3, 4, 7, 0xB, 0xF})
    {
        auto p = OpenServer();
        p->Feed(WsFrame(op, "x"));
        ExpectServerFailed(*p, 1002);
    }
    {
        // Text 帧发给 Binary 端点
        auto p = OpenServer();
        p->Feed(WsFrame(1, "text"));
        ExpectServerFailed(*p, 1003);
    }
    {
        WsServerOptions o;
        o.message_type = WsMessageType::Text;
        auto p = OpenServer(o);
        p->Feed(WsFrame(1, "text"));
        CHECK((p->messages == Strs{"text"}) && !p->host.closed);
        p->Feed(WsFrame(2, "bin")); // Binary 帧发给 Text 端点
        ExpectServerFailed(*p, 1003);
    }
}

void TestServerFragmentation()
{
    {
        auto p = OpenServer();
        p->Feed(WsFrame(2, "ab", false) + WsFrame(9, "p") + WsFrame(0, "cd", false) + WsFrame(10, "") + WsFrame(0, "ef", true));
        CHECK((p->messages == Strs{"abcdef"}));
        CHECK((p->host.events == Strs{"S", "M"})); // Pong 在控制帧到达时立刻回，不等整条消息
        CHECK((WsParseFrames(p->host.sent, false) == std::vector<WsOutFrame>{{10, true, "p"}}));
        // 消息结束后状态复位：下一条单帧消息正常
        p->Feed(WsFrame(2, "next"));
        CHECK(p->messages.size() == 2 && p->messages[1] == "next" && !p->host.closed);
    }
    {
        // 很多个小分片
        auto p = OpenServer();
        std::string all, expect;
        for (int i = 0; i < 1000; ++i)
        {
            all += WsFrame(i == 0 ? 2 : 0, std::string(1, static_cast<char>('a' + i % 26)), i == 999);
            expect.push_back(static_cast<char>('a' + i % 26));
        }
        p->Feed(all);
        CHECK(p->messages.size() == 1 && p->messages[0] == expect);
    }
    {
        auto p = OpenServer();
        p->Feed(WsFrame(0, "orphan")); // 没有在进行的分片消息
        ExpectServerFailed(*p, 1002);
    }
    {
        auto p = OpenServer();
        p->Feed(WsFrame(2, "a", false) + WsFrame(2, "b")); // 分片中间来了新的数据帧
        ExpectServerFailed(*p, 1002);
    }
    {
        auto p = OpenServer();
        p->Feed(WsFrame(9, "x", /*fin=*/false)); // 控制帧不能分片
        ExpectServerFailed(*p, 1002);
    }
}

void TestServerPingPong()
{
    auto p = OpenServer();
    p->Feed(WsFrame(9, "hello"));
    p->Feed(WsFrame(9, ""));
    p->Feed(WsFrame(10, "unsolicited")); // 对端主动发 Pong：忽略
    p->Feed(WsFrame(9, std::string(125, 'x')));
    CHECK((WsParseFrames(p->host.sent, false) ==
           std::vector<WsOutFrame>{{10, true, "hello"}, {10, true, ""}, {10, true, std::string(125, 'x')}}));
    CHECK(p->messages.empty() && !p->host.closed);
    {
        auto q = OpenServer();
        q->Feed(WsHeaderOnly(9, 126)); // 控制帧超过 125 字节
        ExpectServerFailed(*q, 1002);
    }
}

void TestServerCloseHandshake()
{
    {
        auto p = OpenServer();
        p->Feed(WsFrame(8, WsCloseBody(1000, "bye")));
        CHECK((WsParseFrames(p->host.sent, false) == std::vector<WsOutFrame>{{8, true, WsCloseBody(1000)}})); // 回显状态码，不带原因
        CHECK(p->host.closed && p->host.closed->reason == CloseReason::PeerClosed && p->host.closed->code == 1000);
        p->Feed(WsFrame(2, "late"));
        p->Feed(WsFrame(9, "late"));
        CHECK(p->messages.empty() && p->host.close_calls == 1 && WsParseFrames(p->host.sent, false).size() == 1);
    }
    {
        auto p = OpenServer();
        p->Feed(WsFrame(8, ""));
        CHECK((WsParseFrames(p->host.sent, false) == std::vector<WsOutFrame>{{8, true, ""}}));
        CHECK(p->host.closed && p->host.closed->reason == CloseReason::PeerClosed && p->host.closed->code == 1005);
    }
    {
        auto p = OpenServer();
        p->Feed(WsFrame(8, WsCloseBody(4001))); // 业务自定义的关闭码原样带给业务
        CHECK(p->host.closed && p->host.closed->code == 4001 && p->host.closed->reason == CloseReason::PeerClosed);
    }
    {
        // 夹在分片中间的 Close 也有效
        auto p = OpenServer();
        p->Feed(WsFrame(2, "a", false) + WsFrame(8, WsCloseBody(1001)));
        CHECK(p->host.closed && p->host.closed->code == 1001 && p->messages.empty());
    }
    for (const int bad : {999, 1004, 1005, 1006, 1012, 1015, 2999, 5000})
    {
        auto p = OpenServer();
        p->Feed(WsFrame(8, WsCloseBody(bad)));
        ExpectServerFailed(*p, 1002);
    }
    {
        auto p = OpenServer();
        p->Feed(WsFrame(8, "x")); // 状态码只有一半
        ExpectServerFailed(*p, 1002);
    }
    {
        auto p = OpenServer();
        p->Feed(WsFrame(8, WsCloseBody(1000, "\xC0\x80"))); // 原因不是 UTF-8
        ExpectServerFailed(*p, 1007);
    }
    {
        auto p = OpenServer();
        p->Feed(WsFrame(8, WsCloseBody(1000, "\xE4\xBD\xA0\xE5\xA5\xBD"))); // 合法的非 ASCII 原因
        CHECK(p->host.closed && p->host.closed->code == 1000 && p->host.closed->reason == CloseReason::PeerClosed);
    }
}

void TestServerOnClosing()
{
    {
        // 握手期间的 OnClosing：什么也不发
        const WsServerCodecFactory f;
        Peer p(f);
        p.codec->OnClosing();
        CHECK(p.host.sent.empty());
    }
    auto p = OpenServer();
    p->codec->OnClosing();
    CHECK((WsParseFrames(p->host.sent, false) == std::vector<WsOutFrame>{{8, true, WsCloseBody(1001)}}));
    p->codec->OnClosing(); // 只发一次
    CHECK(WsParseFrames(p->host.sent, false).size() == 1);
    p->Feed(WsFrame(2, "last-words")); // 优雅关闭期间对端的最后几条消息照常交付
    CHECK((p->messages == Strs{"last-words"}));
    p->Feed(WsFrame(8, WsCloseBody(1000))); // 对端的应答：不再回显、不再要求关闭（连接已在优雅关闭里）
    CHECK(WsParseFrames(p->host.sent, false).size() == 1 && p->host.close_calls == 0 && !p->error);
    p->Feed(WsFrame(2, "after-close"));
    CHECK(p->messages.size() == 1);
}

void TestServerLimits()
{
    WsServerOptions o;
    o.max_message_bytes = 100;
    {
        auto p = OpenServer(o);
        p->Feed(WsHeaderOnly(2, 101)); // 只有帧头：长度一解出来就拒绝，不等 payload
        ExpectServerFailed(*p, 1009);
    }
    {
        auto p = OpenServer(o);
        p->Feed(WsHeaderOnly(2, std::uint64_t{1} << 40));
        ExpectServerFailed(*p, 1009);
    }
    {
        auto p = OpenServer(o);
        p->Feed(WsFrame(2, std::string(100, 'x'))); // 恰好等于上限
        CHECK(p->messages.size() == 1 && !p->host.closed);
    }
    {
        auto p = OpenServer(o);
        p->Feed(WsFrame(2, std::string(60, 'a'), false));
        p->Feed(WsHeaderOnly(0, 60)); // 累计 120 > 100：第二个分片的帧头一到就拒绝
        ExpectServerFailed(*p, 1009);
    }
    {
        auto p = OpenServer(o);
        p->Feed(WsFrame(2, std::string(40, 'a'), false) + WsFrame(0, std::string(40, 'b'), false) + WsFrame(0, std::string(20, 'c'), true));
        CHECK(p->messages.size() == 1 && p->messages[0].size() == 100 && !p->host.closed);
    }
    {
        // 控制帧最多 125 字节，不受 max_message_bytes 限制
        auto p = OpenServer(o);
        p->Feed(WsFrame(9, std::string(120, 'p')));
        CHECK(p->host.closed == std::nullopt && WsParseFrames(p->host.sent, false).size() == 1);
    }
    {
        WsServerOptions big;
        big.max_message_bytes = 1 << 20;
        auto p = OpenServer(big);
        p->Feed(WsFrame(2, std::string(70000, 'c'), true, true, 0xA1B2C3D4)); // 10 字节头
        CHECK(p->messages.size() == 1 && p->messages[0] == std::string(70000, 'c'));
    }
}

void TestServerUtf8()
{
    WsServerOptions o;
    o.message_type = WsMessageType::Text;
    {
        auto p = OpenServer(o);
        p->Feed(WsFrame(1, "h\xC3\xA9llo \xE4\xBD\xA0\xE5\xA5\xBD \xF0\x9F\x98\x80"));
        CHECK(p->messages.size() == 1 && !p->host.closed);
    }
    {
        auto p = OpenServer(o);
        p->Feed(WsFrame(1, "\xC0\x80"));
        ExpectServerFailed(*p, 1007);
    }
    {
        // 多字节字符被切在两个分片之间：合法
        auto p = OpenServer(o);
        p->Feed(WsFrame(1, "ok\xE4\xBD", false) + WsFrame(0, "\xA0!", true));
        CHECK((p->messages == Strs{"ok\xE4\xBD\xA0!"}) && !p->host.closed);
    }
    {
        // 第二个分片里有非法字节：这一帧到齐就失败，不等 FIN
        auto p = OpenServer(o);
        p->Feed(WsFrame(1, "ok", false) + WsFrame(0, "\xFF", /*fin=*/false));
        ExpectServerFailed(*p, 1007);
    }
    {
        // 消息结束在多字节字符中间
        auto p = OpenServer(o);
        p->Feed(WsFrame(1, "\xE4\xBD", false) + WsFrame(0, "", true));
        ExpectServerFailed(*p, 1007);
    }
    {
        WsServerOptions off = o;
        off.validate_utf8 = false;
        auto p = OpenServer(off);
        p->Feed(WsFrame(1, "\xC0\x80"));
        CHECK(p->messages.size() == 1 && !p->host.closed);
    }
    {
        auto p = OpenServer(); // Binary 端点：内容不是 UTF-8 也照常交付
        p->Feed(WsFrame(2, "\xC0\x80\xFF"));
        CHECK(p->messages.size() == 1 && !p->host.closed);
    }
}

void TestServerEncode()
{
    WsServerOptions o;
    o.max_message_bytes = 1000;
    const WsServerCodecFactory f(o);
    Peer p(f);
    std::array<std::byte, kMaxFrameHeaderSize> h{};
    const auto hdr = [&](std::size_t len) { return std::string(reinterpret_cast<const char*>(h.data()), p.codec->EncodeHeader(len, h)); };
    CHECK(hdr(0) == std::string("\x82\x00", 2));
    CHECK(hdr(125) == std::string("\x82\x7D", 2));
    CHECK(hdr(126) == std::string("\x82\x7E\x00\x7E", 4));
    CHECK(hdr(1000) == std::string("\x82\x7E\x03\xE8", 4));
    CHECK(hdr(1001).empty()); // 超过上限返回 0
    WsServerOptions t;
    t.message_type = WsMessageType::Text;
    t.max_message_bytes = 1 << 20;
    const WsServerCodecFactory ft(t);
    Peer pt(ft);
    CHECK(pt.codec->EncodeHeader(5, h) == 2 && h[0] == std::byte{0x81}); // Text
    const std::size_t n = pt.codec->EncodeHeader(70000, h);
    // 70000 = 0x11170，64 位大端：00 00 00 00 00 01 11 70
    CHECK(n == 10 && h[1] == std::byte{127} && h[2] == std::byte{0} && h[6] == std::byte{0} && h[7] == std::byte{0x01} &&
          h[8] == std::byte{0x11} && h[9] == std::byte{0x70});
}

// ---- 性质：任意切分得到相同的结果 ----

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

struct Outcome
{
    Strs messages;
    std::string sent;
    Strs events;
    std::optional<Closed> closed;
    int handshake_done = 0;
    bool error = false;
};

Outcome Run(const WsServerOptions& o, const std::string& input, const std::vector<std::size_t>& chunks)
{
    const WsServerCodecFactory f(o);
    Peer p(f);
    std::size_t pos = 0;
    for (const std::size_t c : chunks)
    {
        if (pos >= input.size())
            break;
        p.Feed(std::string_view(input).substr(pos, c));
        pos += c;
    }
    if (pos < input.size())
        p.Feed(std::string_view(input).substr(pos));
    return {p.messages, p.host.sent, p.host.events, p.host.closed, p.host.handshake_done, p.error};
}

bool Same(const Outcome& a, const Outcome& b)
{
    const bool closed_same = a.closed.has_value() == b.closed.has_value() &&
                             (!a.closed || (a.closed->reason == b.closed->reason && a.closed->code == b.closed->code));
    // events 里 S 的个数取决于 SendRaw 的次数，与切分无关（每个控制帧一次）；顺序也必须一样
    return a.messages == b.messages && a.sent == b.sent && a.events == b.events && closed_same &&
           a.handshake_done == b.handshake_done && a.error == b.error;
}

void TestChunkingInvariance()
{
    WsServerOptions o;
    o.max_message_bytes = 1 << 20;
    o.subprotocols = {"v1"};
    // 一段完整的会话：握手（带子协议）+ 各种帧（含 4 字节头、10 字节头、分片夹 Ping、空消息）+ Close
    const std::string script =
        WsHandshakeRequest("/ws", "Sec-WebSocket-Protocol: v1\r\n") + WsFrame(2, "a", true, true, 0x01020304) + WsFrame(9, "pp") +
        WsFrame(2, "x", false, true, 0xFFFFFFFF) + WsFrame(9, "mid") + WsFrame(0, "yy", false, true, 0) + WsFrame(0, "zzz", true) +
        WsFrame(2, std::string(300, 'q'), true, true, 0x55555555) + WsFrame(2, "") + WsFrame(2, std::string(70000, 'w'), true, true, 0x0F0F0F0F) +
        WsFrame(8, WsCloseBody(1000, "bye"));

    const Outcome whole = Run(o, script, {script.size()});
    // 先对照手写的期望，再比较各种切分
    CHECK(whole.handshake_done == 1 && !whole.error);
    CHECK(whole.messages.size() == 5 && whole.messages[0] == "a" && whole.messages[1] == "xyyzzz" &&
          whole.messages[2] == std::string(300, 'q') && whole.messages[3].empty() && whole.messages[4] == std::string(70000, 'w'));
    CHECK(whole.sent.starts_with("HTTP/1.1 101 ") && whole.sent.find("Sec-WebSocket-Protocol: v1\r\n") != std::string::npos);
    const std::string frames_out = whole.sent.substr(whole.sent.find("\r\n\r\n") + 4);
    CHECK((WsParseFrames(frames_out, false) == std::vector<WsOutFrame>{{10, true, "pp"}, {10, true, "mid"}, {8, true, WsCloseBody(1000)}}));
    CHECK(whole.closed && whole.closed->reason == CloseReason::PeerClosed && whole.closed->code == 1000);

    // 每个固定大小的切法
    for (const std::size_t c : {1, 2, 3, 5, 7, 13, 64, 1000, 4096})
    {
        std::vector<std::size_t> chunks(script.size() / c + 2, c);
        CHECK(Same(Run(o, script, chunks), whole));
    }
    // 随机切法
    Rng rng{0x9E3779B97F4A7C15ull};
    for (int iter = 0; iter < 60; ++iter)
    {
        std::vector<std::size_t> chunks;
        std::size_t total = 0;
        while (total < script.size())
        {
            const std::size_t c = 1 + rng.Below(iter % 3 == 0 ? 6 : 2000);
            chunks.push_back(c);
            total += c;
        }
        CHECK(Same(Run(o, script, chunks), whole));
    }

    // 出错的会话同样与切分无关：中间夹一个非法帧
    const std::string bad_script =
        WsHandshakeRequest() + WsFrame(2, "ok") + WsFrame(2, "x", true, /*mask=*/false) + WsFrame(2, "never-delivered");
    const Outcome bad_whole = Run(o, bad_script, {bad_script.size()});
    CHECK(bad_whole.messages == Strs{"ok"} && bad_whole.closed && bad_whole.closed->code == 1002);
    for (const std::size_t c : {1, 2, 5, 11})
    {
        std::vector<std::size_t> chunks(bad_script.size() / c + 2, c);
        CHECK(Same(Run(o, bad_script, chunks), bad_whole));
    }
}

// 握手之后喂随机字节（偏向像帧的开头）：不崩、不空转；关闭最多请求一次；之后一律丢弃。
void TestRandomGarbageAfterHandshake()
{
    Rng rng{0x1234567887654321ull};
    for (int iter = 0; iter < 3000; ++iter)
    {
        WsServerOptions o;
        o.max_message_bytes = 2000;
        o.message_type = iter % 2 ? WsMessageType::Text : WsMessageType::Binary;
        const WsServerCodecFactory f(o);
        Peer p(f);
        p.Feed(WsHandshakeRequest());
        CHECK(p.host.handshake_done == 1);
        std::string junk;
        const std::size_t n = rng.Below(300);
        static constexpr unsigned char kStarts[] = {0x81, 0x82, 0x80, 0x88, 0x89, 0x8A, 0x01, 0x00, 0x02};
        for (std::size_t i = 0; i < n; ++i)
        {
            if (i % 9 == 0)
                junk.push_back(static_cast<char>(kStarts[rng.Below(sizeof(kStarts))]));
            else if (i % 9 == 1)
                junk.push_back(static_cast<char>(0x80 | rng.Below(40))); // 带掩码位、长度小：帧常常能完整
            else
                junk.push_back(static_cast<char>(rng()));
        }
        std::size_t pos = 0;
        while (pos < junk.size())
        {
            const std::size_t c = 1 + rng.Below(40);
            p.Feed(std::string_view(junk).substr(pos, c));
            pos += c;
        }
        CHECK(p.host.close_calls <= 1 && p.host.handshake_done == 1);
    }
}

// ---- 客户端 ----

struct ParsedRequest
{
    std::string text;
    ws::HttpHead head;
    ws::ServerRequest req;
};

std::unique_ptr<ParsedRequest> ParseClientRequest(const std::string& sent)
{
    auto r = std::make_unique<ParsedRequest>();
    r->text = sent;
    CHECK(ws::ParseHead(r->text, r->head) == ws::HeadParse::Ok);
    r->req = ws::ClassifyUpgradeRequest(r->head, WsServerOptions{});
    return r;
}

std::string ServerResponse(const std::string& key, const std::string& extra = "")
{
    return "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
           ws::ComputeAccept(key) + "\r\n" + extra + "\r\n";
}

WsClientOptions ClientOpts()
{
    WsClientOptions o;
    o.host = "game.example.com";
    o.path = "/ws?x=1";
    o.origin = "https://o.example.com";
    o.subprotocols = {"v1", "v2"};
    return o;
}

// 客户端握手完成后的 Peer（请求已发出、101 已收到）。sent 清空。
std::unique_ptr<Peer> OpenClient(WsClientOptions o = ClientOpts())
{
    const WsClientCodecFactory f(std::move(o));
    auto p = std::make_unique<Peer>(f);
    const auto req = ParseClientRequest(p->host.sent);
    CHECK(req->req.verdict == ws::RequestVerdict::Upgrade);
    p->Feed(ServerResponse(std::string(req->req.key)));
    CHECK(p->host.handshake_done == 1 && !p->error);
    p->host.sent.clear();
    p->host.events.clear();
    return p;
}

void TestClientUpgradeRequest()
{
    const WsClientCodecFactory f(ClientOpts());
    Peer p(f);
    CHECK(p.codec->traits().needs_handshake && p.codec->traits().encodes_payload);
    CHECK((p.host.events == Strs{"S"})); // Attach 里发出请求
    const auto r = ParseClientRequest(p.host.sent);
    CHECK(r->req.verdict == ws::RequestVerdict::Upgrade);
    CHECK(r->req.host == "game.example.com" && r->req.path == "/ws" && r->req.query == "x=1" && r->req.origin == "https://o.example.com");
    CHECK(r->req.subprotocol_count == 2 && r->req.subprotocols[0] == "v1" && r->req.subprotocols[1] == "v2");
    CHECK(p.host.sent.ends_with("\r\n\r\n") && p.host.handshake_done == 0);

    // 每条连接的 key 不同（16 字节随机）
    const WsClientCodecFactory f2(ClientOpts());
    Peer q(f2);
    CHECK(ParseClientRequest(q.host.sent)->req.key != r->req.key);
}

void TestClientDefaultHost()
{
    WsClientOptions o;
    {
        const WsClientCodecFactory f(o);
        Peer p(f, Endpoint::Tcp("127.0.0.1", 8080));
        CHECK(ParseClientRequest(p.host.sent)->req.host == "127.0.0.1:8080"); // 不带 tcp:// 前缀
    }
    {
        const WsClientCodecFactory f(o);
        Peer p(f, Endpoint::Tcp("::1", 8080));
        CHECK(ParseClientRequest(p.host.sent)->req.host == "[::1]:8080");
    }
    {
        const WsClientCodecFactory f(o);
        Peer p(f); // 没有对端地址
        CHECK(ParseClientRequest(p.host.sent)->req.host == "localhost");
    }
}

void TestClientRejectsUnsafeOptions()
{
    std::vector<WsClientOptions> bad(5);
    bad[0].path = "no-slash";
    bad[1].path = "/a b";
    bad[2].host = "h\r\nX-Injected: 1";
    bad[3].origin = "https://o\r\n";
    bad[4].subprotocols = {"ok", "not a token"};
    for (const auto& o : bad)
    {
        const WsClientCodecFactory f(o);
        Peer p(f);
        CHECK(p.host.sent.empty()); // 不能把含控制字符的东西写上线
        CHECK(p.host.closed && p.host.closed->reason == CloseReason::ProtocolError);
        p.Feed(ServerResponse("whatever")); // 之后什么也不做
        CHECK(p.host.handshake_done == 0 && p.host.sent.empty());
    }
}

void TestClientResponseValidation()
{
    const auto fresh = []
    {
        const WsClientCodecFactory f(ClientOpts());
        auto p = std::make_unique<Peer>(f);
        const auto key = std::string(ParseClientRequest(p->host.sent)->req.key);
        return std::make_pair(std::move(p), key);
    };
    {
        auto [p, key] = fresh();
        p->Feed(ServerResponse(key, "Sec-WebSocket-Protocol: v2\r\n"));
        CHECK(p->host.handshake_done == 1 && !p->error);
    }
    {
        // 一个字节一个字节
        auto [p, key] = fresh();
        const std::string resp = ServerResponse(key);
        for (std::size_t i = 0; i < resp.size(); ++i)
        {
            CHECK(p->host.handshake_done == 0 && !p->error);
            p->Feed(resp.substr(i, 1));
        }
        CHECK(p->host.handshake_done == 1);
    }
    {
        // 101 之后紧跟服务端的第一批帧
        auto [p, key] = fresh();
        p->Feed(ServerResponse(key) + WsFrame(2, "welcome", true, /*mask=*/false));
        CHECK((p->host.events == Strs{"S", "H", "M"}) && (p->messages == Strs{"welcome"}));
    }
    const auto fails = [&](const std::string& tail_headers, bool wrong_key = false, const std::string& status = "101 Switching Protocols")
    {
        auto [p, key] = fresh();
        const std::string accept = wrong_key ? ws::ComputeAccept("AAAAAAAAAAAAAAAAAAAAAA==") : ws::ComputeAccept(key);
        p->Feed("HTTP/1.1 " + status + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n" +
                tail_headers + "\r\n");
        CHECK(p->error && p->host.handshake_done == 0); // Error：连接层关闭、记握手失败
    };
    fails("", /*wrong_key=*/true);
    fails("Sec-WebSocket-Extensions: permessage-deflate\r\n");
    fails("Sec-WebSocket-Protocol: v9\r\n");
    fails("", false, "403 Forbidden");
    fails("", false, "200 OK");
    {
        auto [p, key] = fresh();
        p->Feed("HTTP/1.1 101 X\r\nUpgrade: websocket\r\n\r\n"); // 缺 Connection 与 Accept
        CHECK(p->error);
    }
    {
        // 应答头没有结束、超过上限
        WsClientOptions o = ClientOpts();
        o.max_header_bytes = 128;
        const WsClientCodecFactory f(o);
        Peer p(f);
        p.Feed(std::string(127, 'a'));
        CHECK(!p.error);
        p.Feed("a");
        CHECK(p.error);
    }
}

void TestClientFramesAndMasking()
{
    auto p = OpenClient();
    // 服务端的帧不加掩码
    p->Feed(WsFrame(2, "from-server", true, false));
    CHECK((p->messages == Strs{"from-server"}));
    // Ping → 客户端回一个加掩码的 Pong
    p->Feed(WsFrame(9, "are-you-there", true, false));
    CHECK((WsParseFrames(p->host.sent, /*expect_masked=*/true) == std::vector<WsOutFrame>{{10, true, "are-you-there"}}));
    p->host.sent.clear();
    // 服务端发来加了掩码的帧：协议错误，客户端发出（加掩码的）Close(1002)
    p->Feed(WsFrame(2, "x", true, /*mask=*/true));
    CHECK((WsParseFrames(p->host.sent, true) == std::vector<WsOutFrame>{{8, true, WsCloseBody(1002)}}));
    CHECK(p->host.closed && p->host.closed->code == 1002);
}

void TestClientEncodeAndMask()
{
    WsClientOptions o = ClientOpts();
    o.max_message_bytes = 1 << 20; // 要编 70000 字节（10 字节头），默认的 64KB 上限不够
    auto p = OpenClient(o);
    std::array<std::byte, kMaxFrameHeaderSize> over{};
    CHECK(p->codec->EncodeHeader((std::size_t{1} << 20) + 1, over) == 0); // 超过上限：返回 0，连接层据此关闭
    std::array<std::byte, kMaxFrameHeaderSize> h{};
    std::vector<std::string> keys;
    for (const std::size_t len : {std::size_t{0}, std::size_t{5}, std::size_t{125}, std::size_t{126}, std::size_t{70000}})
    {
        const std::size_t n = p->codec->EncodeHeader(len, h);
        const std::size_t expect = (len < 126 ? 2 : (len <= 0xFFFF ? 4 : 10)) + 4;
        CHECK(n == expect && n <= kMaxFrameHeaderSize);
        CHECK((static_cast<unsigned>(h[0]) & 0x8F) == 0x82 && (static_cast<unsigned>(h[1]) & 0x80) != 0); // FIN | Binary，MASK 位置位
        std::string key(reinterpret_cast<const char*>(h.data()) + n - 4, 4);
        keys.push_back(key);
        std::string payload(len, '\0');
        for (std::size_t i = 0; i < len; ++i)
            payload[i] = static_cast<char>(i * 7 + 1);
        std::string masked = payload;
        p->codec->EncodePayload(std::span(reinterpret_cast<std::byte*>(masked.data()), masked.size()));
        for (std::size_t i = 0; i < len; ++i)
            CHECK(static_cast<unsigned char>(masked[i]) == (static_cast<unsigned char>(payload[i]) ^ static_cast<unsigned char>(key[i % 4])));
    }
    // 掩码键每条消息不同（随机）：5 把键不能全相同
    CHECK(std::any_of(keys.begin(), keys.end(), [&](const std::string& k) { return k != keys[0]; }));
}

// ---- 客户端 ↔ 服务端对跑 ----

void TestClientServerPair()
{
    WsServerOptions so;
    so.max_message_bytes = 1 << 20;
    so.subprotocols = {"v2"};
    WsClientOptions co = ClientOpts();
    co.max_message_bytes = 1 << 20;
    const WsServerCodecFactory sf(so);
    const WsClientCodecFactory cf(co);
    Peer server(sf);
    Peer client(cf);

    const auto pump = [](Peer& from, Peer& to)
    {
        const std::string bytes = std::move(from.host.sent);
        from.host.sent.clear();
        to.Feed(bytes);
        return bytes.size();
    };
    CHECK(pump(client, server) > 0); // 升级请求
    CHECK(pump(server, client) > 0); // 101
    CHECK(server.host.handshake_done == 1 && client.host.handshake_done == 1);
    CHECK(server.host.sent.empty() && client.host.sent.empty());

    const auto send = [](Peer& from, Peer& to, std::string_view payload)
    {
        std::array<std::byte, kMaxFrameHeaderSize> h{};
        const std::size_t n = from.codec->EncodeHeader(payload.size(), h);
        CHECK(n > 0);
        std::string wire(reinterpret_cast<const char*>(h.data()), n);
        std::string body(payload);
        if (from.codec->traits().encodes_payload)
            from.codec->EncodePayload(std::span(reinterpret_cast<std::byte*>(body.data()), body.size()));
        wire += body;
        to.Feed(wire);
    };
    for (const std::size_t len : {std::size_t{0}, std::size_t{1}, std::size_t{125}, std::size_t{126}, std::size_t{65535}, std::size_t{65536}, std::size_t{300000}})
    {
        std::string payload(len, 'a');
        for (std::size_t i = 0; i < len; ++i)
            payload[i] = static_cast<char>('a' + i % 23);
        send(client, server, payload);
        send(server, client, payload);
        CHECK(server.messages.back() == payload && client.messages.back() == payload);
    }
    CHECK(server.messages.size() == 7 && client.messages.size() == 7 && !server.error && !client.error);

    // 服务端优雅关闭：Close(1001) → 客户端回显并要求关闭，OnClosed 带 1001 → 服务端收到应答，不再回
    server.codec->OnClosing();
    pump(server, client);
    CHECK(client.host.closed && client.host.closed->reason == CloseReason::PeerClosed && client.host.closed->code == 1001);
    pump(client, server);
    CHECK(server.host.close_calls == 0 && server.host.sent.empty() && !server.error);
}

} // namespace

int main()
{
    TestServerHandshakeExactResponse();
    TestServerHandshakeByteByByte();
    TestServerPipelinedFrames();
    TestServerRejects();
    TestServerHealth();
    TestServerHeaderLimits();
    TestServerOnUpgrade();
    TestServerOnUpgradeRejects();
    TestServerSubprotocol();
    TestServerDataFrames();
    TestServerFrameViolations();
    TestServerFragmentation();
    TestServerPingPong();
    TestServerCloseHandshake();
    TestServerOnClosing();
    TestServerLimits();
    TestServerUtf8();
    TestServerEncode();
    TestChunkingInvariance();
    TestRandomGarbageAfterHandshake();
    TestClientUpgradeRequest();
    TestClientDefaultHost();
    TestClientRejectsUnsafeOptions();
    TestClientResponseValidation();
    TestClientFramesAndMasking();
    TestClientEncodeAndMask();
    TestClientServerPair();
    std::puts("ws_codec_test: ok");
    return 0;
}
