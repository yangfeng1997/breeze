// HTTP 升级握手：严格的头部解析（obs-fold / 裸 CR LF / 头名里的空白 / 控制字符 / 头数上限 …）、服务端请求判定的状态码表
// 与判定顺序、RFC 6455 §1.3 的 Accept 例子、应答与请求的构造、客户端对 101 应答的校验。
// 无测试框架依赖：失败即 abort。

#include "base/net/ws/internal/http_upgrade.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

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

using namespace breeze;
using namespace breeze::ws;

using Hdrs = std::vector<std::pair<std::string, std::string>>;

constexpr const char* kKey = "dGhlIHNhbXBsZSBub25jZQ=="; // RFC 6455 §1.3 的例子
constexpr const char* kAccept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

Hdrs Base()
{
    return {{"Host", "example.com"},
            {"Upgrade", "websocket"},
            {"Connection", "Upgrade"},
            {"Sec-WebSocket-Key", kKey},
            {"Sec-WebSocket-Version", "13"}};
}

Hdrs Without(Hdrs h, std::string_view name)
{
    std::erase_if(h, [&](const auto& p) { return EqualsNoCase(p.first, name); });
    return h;
}
Hdrs With(Hdrs h, std::string name, std::string value)
{
    h.emplace_back(std::move(name), std::move(value));
    return h;
}
Hdrs Replace(Hdrs h, std::string_view name, std::string value)
{
    return With(Without(std::move(h), name), std::string(name), std::move(value));
}

std::string Text(const std::string& start, const Hdrs& h)
{
    std::string s = start + "\r\n";
    for (const auto& [k, v] : h)
        s += k + ": " + v + "\r\n";
    return s + "\r\n";
}

// 拼请求 → 解析（必须成功）→ 判定。head 要比返回值活得久，所以放在调用方给的槽里。
struct Classified
{
    std::string text;
    HttpHead head;
    ServerRequest req;
};

std::unique_ptr<Classified> Classify(const Hdrs& h,
                                     const WsServerOptions& opts = {},
                                     const std::string& start = "GET /chat?room=7 HTTP/1.1")
{
    auto c = std::make_unique<Classified>();
    c->text = Text(start, h);
    CHECK(ParseHead(c->text, c->head) == HeadParse::Ok);
    c->req = ClassifyUpgradeRequest(c->head, opts);
    return c;
}

int Status(const Hdrs& h, const WsServerOptions& o = {}, const std::string& start = "GET /chat?room=7 HTTP/1.1")
{
    const auto c = Classify(h, o, start);
    CHECK(c->req.verdict == RequestVerdict::Reject);
    return c->req.status;
}

// ---- 解析 ----

void TestParseValid()
{
    const std::string t = "GET /a HTTP/1.1\r\nHost:  example.com \r\nX-Tab:\ta\tb\t\r\nUPGRADE: WebSocket\r\n\r\n";
    HttpHead h;
    CHECK(ParseHead(t, h) == HeadParse::Ok);
    CHECK(h.start_line == "GET /a HTTP/1.1" && h.count == 3);
    CHECK(h.headers[0].name == "Host" && h.headers[0].value == "example.com"); // 首尾空白去掉
    CHECK(h.headers[1].value == "a\tb");                                         // 值内部的 HTAB 保留
    CHECK(h.headers[2].name == "UPGRADE");                                       // 名字保持原样大小写
    CHECK(h.Find("upgrade") != nullptr && h.Find("upgrade")->value == "WebSocket");
    CHECK(h.Find("nope") == nullptr);
}

void TestParseRejects()
{
    HttpHead h;
    const auto bad = [&](std::string t) { return ParseHead(t, h) == HeadParse::Malformed; };
    CHECK(bad("GET / HTTP/1.1\r\nHost: a\r\n"));                       // 没有结束的空行
    CHECK(bad("GET / HTTP/1.1\r\nHost: a\n\n"));                       // 只有 LF
    CHECK(bad("GET / HTTP/1.1\r\nHost: a\r\n\r\nGARBAGE"));            // 空行之后还有东西
    CHECK(bad("\r\nGET / HTTP/1.1\r\nHost: a\r\n\r\n"));              // 开头多了空行
    CHECK(bad("GET / HTTP/1.1\r\nHost: a\nX: b\r\n\r\n"));             // 裸 LF
    CHECK(bad("GET / HTTP/1.1\r\nHost: a\rX: b\r\n\r\n"));             // 裸 CR
    CHECK(bad("GET / HTTP/1.1\r\nHost: a\r\n b\r\n\r\n"));             // obs-fold（空格续行）
    CHECK(bad("GET / HTTP/1.1\r\nHost: a\r\n\tb\r\n\r\n"));            // obs-fold（HTAB 续行）
    CHECK(bad("GET / HTTP/1.1\r\nHost : a\r\n\r\n"));                  // 冒号前有空白
    CHECK(bad("GET / HTTP/1.1\r\n: a\r\n\r\n"));                       // 空头名
    CHECK(bad("GET / HTTP/1.1\r\nNoColon\r\n\r\n"));
    CHECK(bad("GET / HTTP/1.1\r\nBa d: a\r\n\r\n"));                   // 头名里有空格
    CHECK(bad("GET / HTTP/1.1\r\nB@d: a\r\n\r\n"));                    // 头名里有非 token 字符
    std::string with_nul = "GET / HTTP/1.1\r\nHost: a";
    with_nul.push_back('\0');
    CHECK(bad(with_nul + "b\r\n\r\n")); // 值里有 NUL
    CHECK(bad("GET / HTTP/1.1\r\nHost: a\x01\r\n\r\n"));               // 值里有控制字符
    CHECK(bad("GET /\x01 HTTP/1.1\r\nHost: a\r\n\r\n"));               // 起始行里有控制字符
    CHECK(bad(""));
    CHECK(bad("\r\n\r\n"));                                            // 没有起始行
    CHECK(bad("GET / HTTP/1.1\r\nHost: a\r\n\r\n\r\n\r\n"));
}

void TestTooManyHeaders()
{
    std::string t = "GET / HTTP/1.1\r\n";
    for (std::size_t i = 0; i < kMaxHeaders; ++i)
        t += "X" + std::to_string(i) + ": v\r\n";
    HttpHead h;
    CHECK(ParseHead(t + "\r\n", h) == HeadParse::Ok && h.count == kMaxHeaders);
    CHECK(ParseHead(t + "Y: v\r\n\r\n", h) == HeadParse::TooManyHeaders);
}

void TestFindHeadEnd()
{
    const std::string t = "GET / HTTP/1.1\r\nHost: a\r\n\r\nEXTRA";
    const auto bytes = std::as_bytes(std::span(t));
    const std::size_t end = t.find("\r\n\r\n") + 4;
    CHECK(FindHeadEnd(bytes, 1000) == end);
    CHECK(FindHeadEnd(bytes, end) == end);     // 上限刚好等于头长：算找到
    CHECK(FindHeadEnd(bytes, end - 1) == 0);   // 差一个字节：没找到
    CHECK(FindHeadEnd(bytes.first(end - 1), 1000) == 0);
    // 逐包到达：每次从上次确认过的前缀接着找，结果与从头找一样（含结束符被拆在两次之间的所有切法）
    for (std::size_t first_chunk = 0; first_chunk <= end; ++first_chunk)
    {
        const std::size_t r1 = FindHeadEnd(bytes.first(first_chunk), 1000);
        const std::size_t r2 = FindHeadEnd(bytes.first(end), 1000, r1 == 0 ? first_chunk : 0);
        CHECK(r2 == end);
    }
    CHECK(FindHeadEnd({}, 10) == 0);
}

void TestTokens()
{
    CHECK(EqualsNoCase("WebSocket", "websocket") && !EqualsNoCase("websocket", "websockets"));
    CHECK(ListHasToken("keep-alive, Upgrade", "upgrade"));
    CHECK(ListHasToken("Upgrade", "upgrade"));
    CHECK(ListHasToken("a ,\tb , c", "b"));
    CHECK(!ListHasToken("upgrader", "upgrade"));
    CHECK(!ListHasToken("", "upgrade"));
    CHECK(!ListHasToken(",,", "upgrade"));
    CHECK(IsToken("chat.v1+json") && !IsToken("") && !IsToken("a b") && !IsToken("a,b") && !IsToken("a\"b"));
    CHECK(IsSafeHeaderValue("a b") && IsSafeHeaderValue("") && !IsSafeHeaderValue("a\r\nX: y") && !IsSafeHeaderValue("a\nb") &&
          !IsSafeHeaderValue("a\tb"));
    CHECK(IsSafeTarget("/") && IsSafeTarget("/a?b=c") && !IsSafeTarget("") && !IsSafeTarget("a") && !IsSafeTarget("/a b") &&
          !IsSafeTarget("/a\r\n"));
}

// ---- 服务端判定 ----

void TestAcceptRfcExample()
{
    CHECK(ComputeAccept(kKey) == kAccept);
}

void TestClassifyValid()
{
    WsServerOptions o;
    o.path = "/chat";
    const auto c = Classify(With(Replace(Base(), "Connection", "keep-alive, Upgrade"), "Origin", "https://game.example.com"), o);
    CHECK(c->req.verdict == RequestVerdict::Upgrade && c->req.status == 101);
    CHECK(c->req.target == "/chat?room=7" && c->req.path == "/chat" && c->req.query == "room=7");
    CHECK(c->req.host == "example.com" && c->req.origin == "https://game.example.com" && c->req.key == kKey);
    CHECK(c->req.subprotocol_count == 0);

    // 头名与令牌都不分大小写
    const auto c2 = Classify({{"host", "h"},
                              {"UPGRADE", "WebSocket"},
                              {"connection", "UPGRADE"},
                              {"sec-websocket-key", kKey},
                              {"SEC-WEBSOCKET-VERSION", "13"}});
    CHECK(c2->req.verdict == RequestVerdict::Upgrade);
    // 没有 query
    const auto c3 = Classify(Base(), {}, "GET /x HTTP/1.1");
    CHECK(c3->req.path == "/x" && c3->req.query.empty());
    const auto c4 = Classify(Base(), {}, "GET /x? HTTP/1.1");
    CHECK(c4->req.path == "/x" && c4->req.query.empty());
}

void TestClassifyStartLine()
{
    CHECK(Status(Base(), {}, "POST /chat HTTP/1.1") == 405);
    CHECK(Status(Base(), {}, "get /chat HTTP/1.1") == 405); // 方法区分大小写
    CHECK(Status(Base(), {}, "GET /chat HTTP/1.0") == 400);
    CHECK(Status(Base(), {}, "GET /chat HTTP/2.0") == 400);
    CHECK(Status(Base(), {}, "GET /chat HTTP/1.1 extra") == 400);
    CHECK(Status(Base(), {}, "GET  /chat HTTP/1.1") == 400);   // 多一个空格：目标为空
    CHECK(Status(Base(), {}, "GET /chat") == 400);
    CHECK(Status(Base(), {}, "GET") == 400);
    CHECK(Status(Base(), {}, "GET chat HTTP/1.1") == 400);     // 不是 origin-form
    CHECK(Status(Base(), {}, "GET * HTTP/1.1") == 400);
    CHECK(Status(Base(), {}, "GET http://example.com/chat HTTP/1.1") == 400);
    CHECK(Status(Base(), {}, "GE@ /chat HTTP/1.1") == 400);    // 方法不是 token
}

void TestClassifyHeaders()
{
    CHECK(Status(With(Base(), "Content-Length", "0")) == 400);
    CHECK(Status(With(Base(), "Transfer-Encoding", "chunked")) == 400);
    CHECK(Status(Without(Base(), "Host")) == 400);
    CHECK(Status(With(Base(), "Host", "other")) == 400);       // 两个 Host
    CHECK(Status(Replace(Base(), "Host", "")) == 400);

    CHECK(Status(Without(Base(), "Upgrade")) == 426);
    CHECK(Status(Replace(Base(), "Upgrade", "h2c")) == 426);
    CHECK(Status(Without(Base(), "Connection")) == 426);
    CHECK(Status(Replace(Base(), "Connection", "keep-alive")) == 426);
    CHECK(Classify(Replace(Base(), "Upgrade", "foo, websocket"))->req.verdict == RequestVerdict::Upgrade);

    CHECK(Status(Without(Base(), "Sec-WebSocket-Version")) == 426);
    CHECK(Status(Replace(Base(), "Sec-WebSocket-Version", "8")) == 426);
    CHECK(Status(Replace(Base(), "Sec-WebSocket-Version", "13, 8")) == 426);
    CHECK(Status(With(Base(), "Sec-WebSocket-Version", "13")) == 426); // 重复

    CHECK(Status(Without(Base(), "Sec-WebSocket-Key")) == 400);
    CHECK(Status(Replace(Base(), "Sec-WebSocket-Key", "short")) == 400);
    CHECK(Status(Replace(Base(), "Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ=A")) == 400); // 24 字符但不是合法 base64
    CHECK(Status(With(Base(), "Sec-WebSocket-Key", kKey)) == 400);                           // 重复
    CHECK(Status(Replace(Base(), "Sec-WebSocket-Key", "AAAAAAAAAAAAAAAAAAAAAAAA")) == 400);  // 24 字符 base64 但是 18 字节
}

// 判定顺序固定：起始行 → Content-Length → Host → 路由 → Upgrade/Connection → Version → Key → Origin → 子协议。
void TestClassifyOrder()
{
    WsServerOptions o;
    o.path = "/chat";
    // 路径不对、版本不对、key 也不对：先 404
    CHECK(Status(Replace(Replace(Base(), "Sec-WebSocket-Version", "8"), "Sec-WebSocket-Key", "x"), o, "GET /other HTTP/1.1") == 404);
    // 路径对、版本不对、key 也不对：先 426
    CHECK(Status(Replace(Replace(Base(), "Sec-WebSocket-Version", "8"), "Sec-WebSocket-Key", "x"), o, "GET /chat HTTP/1.1") == 426);
    // 版本对、key 不对、Origin 也不对：先 400
    o.allowed_origins = {"https://ok"};
    CHECK(Status(With(Replace(Base(), "Sec-WebSocket-Key", "x"), "Origin", "https://evil"), o, "GET /chat HTTP/1.1") == 400);
    // Content-Length 先于一切（含路由）
    CHECK(Status(With(Base(), "Content-Length", "5"), o, "GET /other HTTP/1.1") == 400);
}

void TestClassifyRouting()
{
    WsServerOptions o;
    o.path = "/chat";
    o.health_path = "/healthz";
    CHECK(Classify(Base(), o, "GET /chat HTTP/1.1")->req.verdict == RequestVerdict::Upgrade);
    CHECK(Status(Base(), o, "GET /chat/ HTTP/1.1") == 404); // 精确匹配
    CHECK(Status(Base(), o, "GET / HTTP/1.1") == 404);
    // 健康检查：普通 GET，不带 Upgrade 头
    const Hdrs plain = {{"Host", "lb"}};
    CHECK(Classify(plain, o, "GET /healthz HTTP/1.1")->req.verdict == RequestVerdict::Health);
    CHECK(Classify(plain, o, "GET /healthz?x=1 HTTP/1.1")->req.verdict == RequestVerdict::Health);
    CHECK(Status(plain, o, "GET /chat HTTP/1.1") == 426);   // 对 ws 路径的普通 GET：要求升级
    CHECK(Status(plain, o, "GET /nope HTTP/1.1") == 404);
    // 健康检查路径上带 Upgrade 头：不是健康检查，按路由走（路径不是 /chat → 404）
    CHECK(Status(Base(), o, "GET /healthz HTTP/1.1") == 404);
    // 没配 path：任意路径都接受升级
    CHECK(Classify(Base(), {}, "GET /anything HTTP/1.1")->req.verdict == RequestVerdict::Upgrade);
    // 没配 health_path：/healthz 与别的路径一样
    WsServerOptions o2;
    CHECK(Status(plain, o2, "GET /healthz HTTP/1.1") == 426);
}

void TestClassifyOrigin()
{
    WsServerOptions o;
    o.allowed_origins = {"https://game.example.com", "https://test.example.com"};
    CHECK(Classify(With(Base(), "Origin", "https://game.example.com"), o)->req.verdict == RequestVerdict::Upgrade);
    CHECK(Classify(With(Base(), "Origin", "HTTPS://GAME.example.com"), o)->req.verdict == RequestVerdict::Upgrade); // 不分大小写
    CHECK(Classify(Base(), o)->req.verdict == RequestVerdict::Upgrade); // 没有 Origin：非浏览器客户端，放行
    CHECK(Status(With(Base(), "Origin", "https://evil.example.com"), o) == 403);
    CHECK(Status(With(Base(), "Origin", "https://game.example.com.evil"), o) == 403); // 不是前缀匹配
    CHECK(Status(With(Base(), "Origin", "null"), o) == 403);
    CHECK(Status(With(With(Base(), "Origin", "https://game.example.com"), "Origin", "https://game.example.com"), o) == 400); // 重复
    // 没配白名单：不检查
    CHECK(Classify(With(Base(), "Origin", "https://anything"))->req.verdict == RequestVerdict::Upgrade);
}

void TestClassifySubprotocols()
{
    const auto c = Classify(With(With(Base(), "Sec-WebSocket-Protocol", "chat, superchat"), "Sec-WebSocket-Protocol", " v2.json ,, v3"));
    CHECK(c->req.verdict == RequestVerdict::Upgrade && c->req.subprotocol_count == 4);
    CHECK(c->req.subprotocols[0] == "chat" && c->req.subprotocols[1] == "superchat" && c->req.subprotocols[2] == "v2.json" &&
          c->req.subprotocols[3] == "v3");
    CHECK(Status(With(Base(), "Sec-WebSocket-Protocol", "bad proto")) == 400); // 不是 token
    CHECK(Status(With(Base(), "Sec-WebSocket-Protocol", "a\"b")) == 400);
    std::string many;
    for (std::size_t i = 0; i <= kMaxSubprotocols; ++i)
        many += (i ? ",p" : "p") + std::to_string(i);
    CHECK(Status(With(Base(), "Sec-WebSocket-Protocol", many)) == 400);

    const std::vector<std::string> pref = {"v3", "chat"};
    CHECK(ChooseSubprotocol(pref, std::span(c->req.subprotocols.data(), c->req.subprotocol_count)) == "v3"); // 取服务端偏好
    const std::vector<std::string> none = {"zzz"};
    CHECK(ChooseSubprotocol(none, std::span(c->req.subprotocols.data(), c->req.subprotocol_count)).empty());
    CHECK(ChooseSubprotocol({}, std::span(c->req.subprotocols.data(), c->req.subprotocol_count)).empty());
    const std::vector<std::string> upper = {"CHAT"};
    CHECK(ChooseSubprotocol(upper, std::span(c->req.subprotocols.data(), c->req.subprotocol_count)).empty()); // 区分大小写
}

// ---- 构造 ----

void TestBuildResponses()
{
    CHECK(BuildUpgradeResponse(kAccept, "") ==
          "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "
          "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n");
    CHECK(BuildUpgradeResponse(kAccept, "chat").find("Sec-WebSocket-Protocol: chat\r\n\r\n") != std::string::npos);

    const std::string r404 = BuildErrorResponse(404);
    CHECK(r404 == "HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
    const std::string r426 = BuildErrorResponse(426);
    CHECK(r426.starts_with("HTTP/1.1 426 Upgrade Required\r\n") && r426.find("Sec-WebSocket-Version: 13\r\n") != std::string::npos);
    CHECK(BuildErrorResponse(405).find("Allow: GET\r\n") != std::string::npos);
    CHECK(BuildErrorResponse(799).starts_with("HTTP/1.1 799 Error\r\n"));
    CHECK(BuildHealthResponse().ends_with("Content-Length: 2\r\n\r\nok"));

    // 构造出来的应答自己能被解析
    HttpHead h;
    const std::string up = BuildUpgradeResponse(kAccept, "chat");
    CHECK(ParseHead(up, h) == HeadParse::Ok && h.start_line == "HTTP/1.1 101 Switching Protocols");
}

void TestBuildRequestRoundTrip()
{
    const std::string req = BuildUpgradeRequest("h.example.com:8443", "/game?x=1", "https://o.example.com", {"a", "b"}, kKey);
    WsServerOptions o;
    HttpHead h;
    CHECK(ParseHead(req, h) == HeadParse::Ok);
    const ServerRequest r = ClassifyUpgradeRequest(h, o);
    CHECK(r.verdict == RequestVerdict::Upgrade && r.path == "/game" && r.query == "x=1" && r.host == "h.example.com:8443");
    CHECK(r.origin == "https://o.example.com" && r.key == kKey && r.subprotocol_count == 2 && r.subprotocols[1] == "b");
    // 没有 origin / 子协议时不写这两个头
    const std::string min = BuildUpgradeRequest("h", "/", "", {}, kKey);
    CHECK(min.find("Origin") == std::string::npos && min.find("Sec-WebSocket-Protocol") == std::string::npos);
}

// ---- 客户端校验应答 ----

ResponseCheck Check(const std::string& start, const Hdrs& h, const std::vector<std::string>& offered = {})
{
    const std::string t = Text(start, h);
    HttpHead head;
    CHECK(ParseHead(t, head) == HeadParse::Ok);
    return ValidateUpgradeResponse(head, kAccept, offered);
}

Hdrs GoodResponseHeaders()
{
    return {{"Upgrade", "websocket"}, {"Connection", "Upgrade"}, {"Sec-WebSocket-Accept", kAccept}};
}

void TestValidateResponse()
{
    CHECK(Check("HTTP/1.1 101 Switching Protocols", GoodResponseHeaders()).ok);
    CHECK(Check("HTTP/1.1 101", GoodResponseHeaders()).ok);   // 没有原因短语
    CHECK(Check("HTTP/1.1 101 ", GoodResponseHeaders()).ok);
    CHECK(Check("HTTP/1.1 101 X", Replace(GoodResponseHeaders(), "upgrade", "WebSocket")).ok);

    const auto not101 = Check("HTTP/1.1 403 Forbidden", GoodResponseHeaders());
    CHECK(!not101.ok && not101.status == 403);
    CHECK(Check("HTTP/1.1 200 OK", GoodResponseHeaders()).status == 200);
    CHECK(!Check("HTTP/1.0 101 Switching Protocols", GoodResponseHeaders()).ok);
    CHECK(!Check("HTTP/1.1 1010 X", GoodResponseHeaders()).ok);
    CHECK(!Check("HTTP/1.1 10x X", GoodResponseHeaders()).ok);
    CHECK(Check("garbage", GoodResponseHeaders()).status == 0);

    CHECK(!Check("HTTP/1.1 101 X", Without(GoodResponseHeaders(), "Upgrade")).ok);
    CHECK(!Check("HTTP/1.1 101 X", Replace(GoodResponseHeaders(), "Upgrade", "h2c")).ok);
    CHECK(!Check("HTTP/1.1 101 X", Without(GoodResponseHeaders(), "Connection")).ok);
    CHECK(!Check("HTTP/1.1 101 X", Without(GoodResponseHeaders(), "Sec-WebSocket-Accept")).ok);
    CHECK(!Check("HTTP/1.1 101 X", Replace(GoodResponseHeaders(), "Sec-WebSocket-Accept", "s3pPLMBiTxaQ9kYGzzhZRbK+xOp=")).ok);
    CHECK(!Check("HTTP/1.1 101 X", Replace(GoodResponseHeaders(), "Sec-WebSocket-Accept", "S3PPLMBITXAQ9KYGZZHZRBK+XOO=")).ok); // 大小写敏感
    CHECK(!Check("HTTP/1.1 101 X", With(GoodResponseHeaders(), "Sec-WebSocket-Accept", kAccept)).ok);                         // 重复
    // 我们没有提供 extension，应答里出现就是服务端违规
    CHECK(!Check("HTTP/1.1 101 X", With(GoodResponseHeaders(), "Sec-WebSocket-Extensions", "permessage-deflate")).ok);

    // 子协议：应答里的必须是我们提供的之一；没提供过就不能出现；不出现是允许的
    CHECK(Check("HTTP/1.1 101 X", With(GoodResponseHeaders(), "Sec-WebSocket-Protocol", "chat"), {"chat", "v2"}).ok);
    CHECK(!Check("HTTP/1.1 101 X", With(GoodResponseHeaders(), "Sec-WebSocket-Protocol", "other"), {"chat", "v2"}).ok);
    CHECK(!Check("HTTP/1.1 101 X", With(GoodResponseHeaders(), "Sec-WebSocket-Protocol", "chat")).ok);
    CHECK(Check("HTTP/1.1 101 X", GoodResponseHeaders(), {"chat"}).ok);
    CHECK(!Check("HTTP/1.1 101 X", With(With(GoodResponseHeaders(), "Sec-WebSocket-Protocol", "chat"), "Sec-WebSocket-Protocol", "chat"), {"chat"}).ok);
}

} // namespace

int main()
{
    TestParseValid();
    TestParseRejects();
    TestTooManyHeaders();
    TestFindHeadEnd();
    TestTokens();
    TestAcceptRfcExample();
    TestClassifyValid();
    TestClassifyStartLine();
    TestClassifyHeaders();
    TestClassifyOrder();
    TestClassifyRouting();
    TestClassifyOrigin();
    TestClassifySubprotocols();
    TestBuildResponses();
    TestBuildRequestRoundTrip();
    TestValidateResponse();
    std::puts("http_upgrade_test: ok");
    return 0;
}
