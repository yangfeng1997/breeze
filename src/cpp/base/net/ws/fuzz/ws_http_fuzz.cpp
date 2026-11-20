// libFuzzer：HTTP 升级握手的解析与判定（服务端请求、客户端对 101 应答的校验）。默认不构建：-DBREEZE_BUILD_FUZZ=ON，
// 配合 -DBREEZE_SANITIZE=fuzzer-no-link,address,undefined，需要 clang。
//
// 检查的不变量：FindHeadEnd 的结果是第一个 CRLFCRLF；ParseHead 成功时所有视图都落在输入里、头名是 token、值里没有控制字符；
// 判定结果自洽（Upgrade 时 key 恰好 24 字符、path / query / target 对得上；Reject 的状态码在合理范围）；
// 解析成功的头再做客户端应答校验不崩。另外把输入当作服务端 / 客户端握手阶段的字节流整个喂给 codec（WsPeer 检查契约）。
//
// 分工同 ws_decode_fuzz：查崩溃 / 越界 / 不变量 / 握手阶段的切分不变，不查语义。验证过：注入"结束符扫描起点错位"几十秒内被抓住。
//
// 跑法：  ./ws_http_fuzz corpus/ -max_len=2048
// 实测（clang 23、asan + ubsan）：120 秒 518 万次执行，零崩溃零不一致。

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "base/net/testing/ws_codec_harness.h"
#include "base/net/testing/ws_test_util.h"
#include "base/net/ws/internal/http_upgrade.h"
#include "base/net/ws/ws_codec.h"

using namespace breeze;
using namespace breeze::testing;

namespace
{

bool Inside(std::string_view outer, std::string_view inner)
{
    return inner.empty() || (inner.data() >= outer.data() && inner.data() + inner.size() <= outer.data() + outer.size());
}

void CheckHead(std::string_view text, const ws::HttpHead& head)
{
    CHECK(Inside(text, head.start_line) && head.count <= ws::kMaxHeaders);
    CHECK(head.start_line.find_first_of("\r\n") == std::string_view::npos);
    for (std::size_t i = 0; i < head.count; ++i)
    {
        const WsHeader& h = head.headers[i];
        CHECK(Inside(text, h.name) && Inside(text, h.value));
        CHECK(ws::IsToken(h.name));
        for (const char c : h.value)
            CHECK(c == '\t' || (static_cast<unsigned char>(c) >= 0x20 && c != 0x7F));
        CHECK(h.value.empty() || (h.value.front() != ' ' && h.value.front() != '\t' && h.value.back() != ' ' && h.value.back() != '\t'));
    }
}

void CheckRequest(std::string_view text, const ws::HttpHead& head, const WsServerOptions& opts)
{
    const ws::ServerRequest r = ws::ClassifyUpgradeRequest(head, opts);
    if (r.verdict == ws::RequestVerdict::Reject)
    {
        CHECK(r.status >= 400 && r.status <= 499);
        return;
    }
    CHECK(Inside(text, r.target) && Inside(text, r.path) && Inside(text, r.query) && Inside(text, r.host));
    CHECK(!r.target.empty() && r.target.front() == '/' && r.target.substr(0, r.path.size()) == r.path);
    if (r.verdict == ws::RequestVerdict::Upgrade)
    {
        CHECK(r.key.size() == 24 && Inside(text, r.key) && r.subprotocol_count <= ws::kMaxSubprotocols);
        CHECK(ws::ComputeAccept(r.key).size() == 28);
        for (std::size_t i = 0; i < r.subprotocol_count; ++i)
            CHECK(ws::IsToken(r.subprotocols[i]));
    }
}

struct HandshakeOutcome
{
    std::string sent; // 客户端的请求带随机 key，不参与比较
    std::vector<std::string> events;
    bool closed = false;
    int code = 0;
    CloseReason reason = CloseReason::PeerClosed;
    int handshake_done = 0;
    bool error = false;
};

// 把握手阶段的字节流喂给 codec；whole = false 时按随机块大小切分。
HandshakeOutcome RunHandshake(bool client, const std::string& text, std::uint64_t seed, bool whole)
{
    WsServerOptions so;
    so.max_header_bytes = 1024; // 小一点，让"头太大"路径常被走到
    WsClientOptions co;
    co.host = "fuzz.example.com";
    co.max_header_bytes = 1024;
    const WsServerCodecFactory sf(so);
    const WsClientCodecFactory cf(co);
    WsPeer p(client ? static_cast<const IFrameCodecFactory&>(cf) : sf);
    if (client)
        p.host.sent.clear(); // 请求带随机 key
    if (whole)
    {
        p.Feed(text);
    }
    else
    {
        std::uint64_t s = seed | 1;
        const auto next = [&s]
        {
            s ^= s << 13;
            s ^= s >> 7;
            s ^= s << 17;
            return s;
        };
        std::size_t pos = 0;
        while (pos < text.size())
        {
            const std::size_t n = 1 + static_cast<std::size_t>(next() % ((next() & 1) ? 5 : 300));
            p.Feed(std::string_view(text).substr(pos, n));
            pos += n;
        }
    }
    HandshakeOutcome o;
    o.sent = client ? std::string() : p.host.sent;
    o.events = p.host.events;
    o.closed = p.host.closed.has_value();
    if (p.host.closed)
    {
        o.code = p.host.closed->code;
        o.reason = p.host.closed->reason;
    }
    o.handshake_done = p.host.handshake_done;
    o.error = p.error;
    CHECK(p.host.close_calls <= 1 && p.host.handshake_done <= 1);
    return o;
}

void CheckHandshakeChunking(bool client, const std::string& text)
{
    std::uint64_t seed = 0xCBF29CE484222325ull;
    for (const char ch : text)
        seed = (seed ^ static_cast<unsigned char>(ch)) * 0x100000001B3ull;
    const HandshakeOutcome a = RunHandshake(client, text, seed, true);
    const HandshakeOutcome b = RunHandshake(client, text, seed, false);
    CHECK(a.sent == b.sent && a.events == b.events && a.closed == b.closed && a.code == b.code && a.reason == b.reason);
    CHECK(a.handshake_done == b.handshake_done && a.error == b.error);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const std::string text(reinterpret_cast<const char*>(data), size);
    const auto bytes = std::as_bytes(std::span(text));

    // FindHeadEnd：结果是第一个 CRLFCRLF 之后的位置
    const std::size_t end = ws::FindHeadEnd(bytes, 4096);
    const std::size_t limit = std::min<std::size_t>(size, 4096);
    const std::size_t first = text.find("\r\n\r\n");
    if (first == std::string::npos || first + 4 > limit)
        CHECK(end == 0);
    else
        CHECK(end == first + 4);

    // 把输入当作头（原样、以及补上结束符）解析并判定
    for (const std::string& t : {text, text + "\r\n\r\n"})
    {
        ws::HttpHead head;
        if (ws::ParseHead(t, head) != ws::HeadParse::Ok)
            continue;
        CheckHead(t, head);
        WsServerOptions plain;
        CheckRequest(t, head, plain);
        WsServerOptions strict;
        strict.path = "/ws";
        strict.health_path = "/healthz";
        strict.allowed_origins = {"https://a.example"};
        strict.subprotocols = {"v1", "v2"};
        CheckRequest(t, head, strict);
        (void)ws::ValidateUpgradeResponse(head, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", {"v1"});
    }

    // 整个输入当作握手阶段的字节流喂给 codec：服务端（请求）与客户端（应答）各一遍；
    // 每遍都跑"整块喂"与"随机切分喂"，结果必须相同（握手阶段的任意切分不变）。
    CheckHandshakeChunking(/*client=*/false, text);
    CheckHandshakeChunking(/*client=*/true, text);
    return 0;
}
