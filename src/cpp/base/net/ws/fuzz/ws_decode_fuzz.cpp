// libFuzzer：WebSocket codec 的解码路径（握手之后的帧流）。默认不构建：-DBREEZE_BUILD_FUZZ=ON，
// 配合 -DBREEZE_SANITIZE=fuzzer-no-link,address,undefined（让库本身也被插桩），需要 clang。
//
// 输入：第 0 字节是配置（消息类型、UTF-8 校验、长度上限档位、服务端 / 客户端角色），其余是对端发来的字节流。
// 每个输入跑两遍——整块喂入与随机切分喂入——结果（消息序列、写出的字节、事件顺序、关闭原因、Error）必须完全相同：
// 这是"任意切分不变"的性质，fuzz 下比固定用例强得多。WsPeer::Feed 里同时检查 Decode 的契约
// （NeedMore / Error 不改缓冲、Consumed 必须推进），违约直接 abort。
//
// 分工：fuzz 查的是崩溃 / 越界 / 契约 / 切分不变；"语义对不对"（例如分片消息结束后状态是否复位）不会因切分而变，fuzz 查不到，
// 由 ws_codec_test 与 Autobahn 把关。验证过：注入"帧完整性判断差一个字节"几秒内被抓住，注入"重组缓冲不复位"则不会（预期）。
//
// 跑法：  ./ws_decode_fuzz corpus/ -max_len=4096 -runs=...      种子语料可以从 ws_test_util.h 的 WsFrame 生成。
// 实测（clang 23、asan + ubsan）：120 秒 131 万次执行，零崩溃零不一致。

#include <cstdint>
#include <cstring>
#include <memory>
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

struct Outcome
{
    std::vector<std::string> messages;
    std::string sent;
    std::vector<std::string> events;
    bool closed = false;
    CloseReason reason = CloseReason::PeerClosed;
    int code = 0;
    int close_calls = 0;
    bool error = false;
};

struct Config
{
    bool client = false;
    WsMessageType type = WsMessageType::Binary;
    bool validate_utf8 = true;
    std::size_t max_message = 256;
};

// 握手完成后的 Peer；客户端角色要先用对应的 101 应答完成握手。
std::unique_ptr<WsPeer> Open(const Config& c)
{
    if (!c.client)
    {
        WsServerOptions o;
        o.message_type = c.type;
        o.validate_utf8 = c.validate_utf8;
        o.max_message_bytes = c.max_message;
        const WsServerCodecFactory f(std::move(o));
        auto p = std::make_unique<WsPeer>(f);
        p->Feed(WsHandshakeRequest());
        CHECK(p->host.handshake_done == 1 && !p->error);
        p->host.sent.clear();
        p->host.events.clear();
        return p;
    }
    WsClientOptions o;
    o.host = "fuzz.example.com";
    o.message_type = c.type;
    o.validate_utf8 = c.validate_utf8;
    o.max_message_bytes = c.max_message;
    const WsClientCodecFactory f(std::move(o));
    auto p = std::make_unique<WsPeer>(f);
    ws::HttpHead head;
    CHECK(ws::ParseHead(p->host.sent, head) == ws::HeadParse::Ok);
    const ws::ServerRequest req = ws::ClassifyUpgradeRequest(head, WsServerOptions{});
    CHECK(req.verdict == ws::RequestVerdict::Upgrade);
    const std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                             ws::ComputeAccept(req.key) + "\r\n\r\n";
    p->Feed(resp);
    CHECK(p->host.handshake_done == 1 && !p->error);
    p->host.sent.clear();
    p->host.events.clear();
    return p;
}

// 客户端写出的帧带随机掩码键，两次运行的原始字节本来就不同：改比较解掩码后的帧内容。服务端的帧不加掩码，直接比字节。
std::string Normalize(bool client, const std::string& sent)
{
    if (!client)
        return sent;
    std::string out;
    for (const WsOutFrame& f : WsParseFrames(sent, /*expect_masked=*/true))
    {
        out.push_back(static_cast<char>(f.opcode));
        out.push_back(f.fin ? 1 : 0);
        out += std::to_string(f.payload.size());
        out.push_back(':');
        out += f.payload;
    }
    return out;
}

Outcome Run(const Config& c, const std::string& stream, std::uint64_t chunk_seed, bool whole)
{
    auto p = Open(c);
    if (whole)
    {
        p->Feed(stream);
    }
    else
    {
        std::uint64_t s = chunk_seed | 1; // xorshift64
        const auto next = [&s]
        {
            s ^= s << 13;
            s ^= s >> 7;
            s ^= s << 17;
            return s;
        };
        std::size_t pos = 0;
        while (pos < stream.size())
        {
            const std::size_t n = 1 + static_cast<std::size_t>(next() % ((next() & 1) ? 7 : 600));
            p->Feed(std::string_view(stream).substr(pos, n));
            pos += n;
        }
    }
    Outcome o;
    o.messages = p->messages;
    o.sent = Normalize(c.client, p->host.sent);
    o.events = p->host.events;
    o.closed = p->host.closed.has_value();
    if (p->host.closed)
    {
        o.reason = p->host.closed->reason;
        o.code = p->host.closed->code;
    }
    o.close_calls = p->host.close_calls;
    o.error = p->error;
    CHECK(o.close_calls <= 1);
    return o;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size < 2)
        return 0;
    Config c;
    const std::uint8_t cfg = data[0];
    c.type = (cfg & 1) ? WsMessageType::Text : WsMessageType::Binary;
    c.validate_utf8 = (cfg & 2) != 0;
    static constexpr std::size_t kMax[] = {64, 1024, 16 * 1024, 1u << 20};
    c.max_message = kMax[(cfg >> 2) & 3];
    c.client = (cfg & 16) != 0;
    const std::string stream(reinterpret_cast<const char*>(data + 1), size - 1);

    std::uint64_t seed = 0x9E3779B97F4A7C15ull;
    for (const char ch : stream)
        seed = (seed ^ static_cast<unsigned char>(ch)) * 0x100000001B3ull;

    const Outcome whole = Run(c, stream, seed, /*whole=*/true);
    const Outcome chunked = Run(c, stream, seed, /*whole=*/false);
    CHECK(whole.messages == chunked.messages);
    CHECK(whole.sent == chunked.sent);
    CHECK(whole.events == chunked.events);
    CHECK(whole.closed == chunked.closed && whole.reason == chunked.reason && whole.code == chunked.code);
    CHECK(whole.error == chunked.error);
    return 0;
}
