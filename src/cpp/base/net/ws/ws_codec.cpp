#include "base/net/ws/ws_codec.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "base/logger/logger_macros.h"
#include "base/net/codec/codec_host_interface.h"
#include "base/net/ws/internal/http_upgrade.h"
#include "base/net/ws/internal/ws_frame.h"
#include "base/random/random_util.h"
#include "base/string/base64.h"
#include "base/string/utf8.h"

namespace breeze
{

namespace ws
{

// 一个端点上全部连接共享。服务端 / 客户端各用自己那份选项，公共字段归一到前面几个，帧路径只读它们。
struct CodecShared
{
    bool server = false;
    bool valid = true; // 客户端选项是否安全（见 WsClientCodecFactory）
    WsMessageType type = WsMessageType::Binary;
    bool validate_utf8 = true;
    std::size_t max_header_bytes = 0;
    std::size_t max_message_bytes = 0;
    WsServerOptions srv;
    WsClientOptions cli;
};

} // namespace ws

namespace
{

using ws::Opcode;

constexpr std::size_t kMaxMessageCap = std::size_t{1} << 30; // 配置的上限：再大没有意义，也防 size_t 运算溢出
constexpr std::size_t kMaxHeaderCap = std::size_t{1} << 20;

enum CloseCode : std::uint16_t
{
    kGoingAway = 1001,
    kProtocolError = 1002,
    kUnsupportedData = 1003,
    kInvalidPayload = 1007,
    kMessageTooBig = 1009,
};

std::array<std::byte, 4> RandomMaskKey() noexcept
{
    const auto r = static_cast<std::uint32_t>(ThreadRandom()());
    std::array<std::byte, 4> key;
    std::memcpy(key.data(), &r, 4);
    return key;
}

class WsCodec final : public IFrameCodec
{
public:
    WsCodec(std::shared_ptr<const ws::CodecShared> shared)
        : shared_(std::move(shared))
        , server_(shared_->server)
    {
    }

    CodecTraits traits() const noexcept override { return {.needs_handshake = true, .encodes_payload = !server_}; }

    void Attach(ICodecHost& host) override
    {
        host_ = &host;
        if (!server_)
            SendClientUpgrade();
    }

    DecodeResult Decode(std::span<std::byte> data, FrameSink sink, std::size_t& used) override
    {
        used = 0;
        if (data.empty()) [[unlikely]]
            return DecodeResult::NeedMore;
        if (state_ != State::Handshaking) [[likely]]
            return DecodeFrame(data, sink, used);
        return server_ ? ServerHandshake(data, used) : ClientHandshake(data, used);
    }

    std::size_t EncodeHeader(std::size_t payload_len, std::span<std::byte, kMaxFrameHeaderSize> out) override
    {
        if (payload_len > shared_->max_message_bytes)
            return 0;
        const std::array<std::byte, 4>* key = nullptr;
        if (!server_)
        {
            mask_key_ = RandomMaskKey(); // 配对的 EncodePayload 紧跟在后面，用的是这一把
            key = &mask_key_;
        }
        return ws::EncodeFrameHeader(true, DataOpcode(), payload_len, key, out.first<ws::kMaxHeaderSize>());
    }

    void EncodePayload(std::span<std::byte> payload) noexcept override { ws::ApplyMask(payload, mask_key_); }

    void OnClosing() override
    {
        if (state_ != State::Open)
            return;
        state_ = State::CloseSent; // 我们先发起的关闭：对端的 Close 是应答，不再回显
        SendClose(kGoingAway);
    }

private:
    enum class State : std::uint8_t
    {
        Handshaking,
        Open,
        CloseSent, // 已发出 Close 帧（OnClosing），数据照常收，等对端的 Close / FIN
        Dead,      // 要关了（已发 Close / 错误应答、CloseAfterFlush 过）：之后的字节一律丢弃
    };

    // 分片消息的重组缓冲与 UTF-8 状态：绝大多数连接一辈子不分片，第一次分片时才分配。
    struct Fragments
    {
        std::vector<std::byte> data;
        str::Utf8Validator utf8;
    };

    Opcode DataOpcode() const noexcept
    {
        return shared_->type == WsMessageType::Text ? Opcode::Text : Opcode::Binary;
    }

    // ---- 发控制帧 / 关闭 ----

    // 控制帧经 SendRaw 写出，与 Send 保序。客户端的帧必须加掩码。
    void SendControl(Opcode op, std::span<const std::byte> payload)
    {
        std::array<std::byte, ws::kMaxHeaderSize + ws::kMaxControlPayload> buf;
        std::array<std::byte, 4> key{};
        if (!server_)
            key = RandomMaskKey();
        const std::size_t h = ws::EncodeFrameHeader(
            true, op, payload.size(), server_ ? nullptr : &key, std::span<std::byte, ws::kMaxHeaderSize>(buf.data(), ws::kMaxHeaderSize));
        if (!payload.empty())
            std::memcpy(buf.data() + h, payload.data(), payload.size());
        if (!server_)
            ws::ApplyMask(std::span(buf.data() + h, payload.size()), key);
        host_->SendRaw(std::span<const std::byte>(buf.data(), h + payload.size()));
    }

    void SendClose(std::uint16_t code)
    {
        const std::byte p[2] = {static_cast<std::byte>(code >> 8), static_cast<std::byte>(code)};
        SendControl(Opcode::Close, p);
    }

    // 协议违规：发 Close(code)，写完再关，丢弃此后的一切字节。关闭码作为 OnClosed 的 sys_errno 交给业务。
    DecodeResult Fail(std::uint16_t code, std::size_t avail, std::size_t& used)
    {
        LOG_DEBUG("ws protocol violation, closing with code {}", code);
        if (state_ == State::Open) // CloseSent：Close 已经发过，连接也已在优雅关闭里，不再发第二个
        {
            SendClose(code);
            host_->CloseAfterFlush(CloseReason::ProtocolError, code);
        }
        state_ = State::Dead;
        used = avail;
        return DecodeResult::Consumed;
    }

    // ---- 帧 ----

    DecodeResult DecodeFrame(std::span<std::byte> data, FrameSink sink, std::size_t& used)
    {
        if (state_ == State::Dead) [[unlikely]]
        {
            used = data.size();
            return DecodeResult::Consumed;
        }

        ws::FrameHeader h;
        switch (ws::ParseFrameHeader(data, /*expect_masked=*/server_, h))
        {
            case ws::HeaderParse::NeedMore:
                return DecodeResult::NeedMore;
            case ws::HeaderParse::Error:
                return Fail(kProtocolError, data.size(), used);
            case ws::HeaderParse::Ok:
                break;
        }

        // 上限在帧头一解出来就检查，先于等待 payload：恶意的超大长度不会让缓冲无限增长。控制帧最多 125 字节，不受它限制。
        const std::size_t max = shared_->max_message_bytes;
        const bool control = ws::IsControl(h.opcode);
        if (!control)
        {
            const std::size_t have = frag_ ? frag_->data.size() : 0; // 不变量：have <= max
            if (h.payload_len > max - have)
                return Fail(kMessageTooBig, data.size(), used);
        }
        const std::size_t total = h.header_size + static_cast<std::size_t>(h.payload_len);
        if (data.size() < total)
            return DecodeResult::NeedMore;

        // 帧完整了，可以原地去掩码（契约：NeedMore / Error 之前不得改动缓冲）。
        const std::span<std::byte> payload = data.subspan(h.header_size, static_cast<std::size_t>(h.payload_len));
        if (h.masked)
            ws::ApplyMask(payload, h.mask_key);
        used = total;

        switch (h.opcode)
        {
            case Opcode::Binary:
            case Opcode::Text:
                return OnDataFrame(h, payload, sink, data.size(), used);
            case Opcode::Continuation:
                return OnContinuation(h, payload, sink, data.size(), used);
            case Opcode::Ping:
                if (state_ == State::Open)
                    SendControl(Opcode::Pong, payload);
                return DecodeResult::Consumed;
            case Opcode::Pong:
                return DecodeResult::Consumed;
            case Opcode::Close:
                return OnClose(payload, data.size(), used);
        }
        return Fail(kProtocolError, data.size(), used); // ParseFrameHeader 已排除其它 opcode
    }

    DecodeResult OnDataFrame(const ws::FrameHeader& h,
                             std::span<std::byte> payload,
                             FrameSink sink,
                             std::size_t avail,
                             std::size_t& used)
    {
        if (frag_) // 上一条分片消息还没结束就来了新的数据帧
            return Fail(kProtocolError, avail, used);
        if (h.opcode != DataOpcode())
            return Fail(kUnsupportedData, avail, used);
        const bool check_utf8 = h.opcode == Opcode::Text && shared_->validate_utf8;
        if (h.fin)
        {
            // 常见路径：单帧消息，零拷贝，视图直接指向读缓冲。
            if (check_utf8 && !str::IsValidUtf8(payload))
                return Fail(kInvalidPayload, avail, used);
            sink(payload);
            return DecodeResult::Ok;
        }
        auto frag = std::make_unique<Fragments>();
        frag->data.assign(payload.begin(), payload.end());
        if (check_utf8 && !frag->utf8.Feed(payload))
            return Fail(kInvalidPayload, avail, used);
        frag_ = std::move(frag);
        return DecodeResult::Consumed;
    }

    DecodeResult OnContinuation(const ws::FrameHeader& h,
                                std::span<std::byte> payload,
                                FrameSink sink,
                                std::size_t avail,
                                std::size_t& used)
    {
        if (!frag_) // 没有在进行的分片消息
            return Fail(kProtocolError, avail, used);
        const bool check_utf8 = shared_->type == WsMessageType::Text && shared_->validate_utf8;
        if (check_utf8 && !frag_->utf8.Feed(payload)) // 每帧到齐就校验，尽早失败，不等整条消息
            return Fail(kInvalidPayload, avail, used);
        frag_->data.insert(frag_->data.end(), payload.begin(), payload.end());
        if (!h.fin)
            return DecodeResult::Consumed;
        if (check_utf8 && !frag_->utf8.AtBoundary()) // 消息结束在多字节字符中间
            return Fail(kInvalidPayload, avail, used);
        // 先把重组缓冲摘下来：sink 里业务可能关闭连接，不能让它在我们手里被改动。
        const std::unique_ptr<Fragments> frag = std::move(frag_);
        sink(std::span<const std::byte>(frag->data));
        return DecodeResult::Ok;
    }

    DecodeResult OnClose(std::span<std::byte> payload, std::size_t avail, std::size_t& used)
    {
        std::uint16_t code = ws::kCloseNoStatus;
        if (payload.size() == 1)
            return Fail(kProtocolError, avail, used); // 状态码只有一半
        if (payload.size() >= 2)
        {
            code = static_cast<std::uint16_t>((static_cast<std::uint16_t>(payload[0]) << 8) | static_cast<std::uint16_t>(payload[1]));
            if (!ws::IsValidCloseCode(code))
                return Fail(kProtocolError, avail, used);
            if (!str::IsValidUtf8(payload.subspan(2))) // 关闭原因必须是 UTF-8
                return Fail(kInvalidPayload, avail, used);
        }
        if (state_ == State::Open)
        {
            // 对端先发起：回显状态码（不带原因），写完再关；OnClosed 带出对端的关闭码。
            SendControl(Opcode::Close, payload.first(std::min<std::size_t>(payload.size(), 2)));
            state_ = State::Dead;
            host_->CloseAfterFlush(CloseReason::PeerClosed, code);
        }
        else
        {
            // 我们先发起（CloseSent）：这是对端的应答，连接已在 Disconnecting，等对端 FIN 收尾。
            state_ = State::Dead;
        }
        return DecodeResult::Consumed;
    }

    // ---- 握手：服务端 ----

    // 拒绝升级：写 HTTP 错误应答，写完再关。status 在握手期不会交给业务（没有 OnConnected 也就没有 OnClosed），只进日志。
    DecodeResult RejectUpgrade(int status, std::size_t avail, std::size_t& used)
    {
        LOG_DEBUG("ws upgrade rejected with HTTP {}", status);
        const std::string resp = ws::BuildErrorResponse(status);
        host_->SendRaw(std::as_bytes(std::span(resp)));
        state_ = State::Dead;
        host_->CloseAfterFlush(CloseReason::ProtocolError, status);
        used = avail;
        return DecodeResult::Consumed;
    }

    DecodeResult ServerHandshake(std::span<std::byte> data, std::size_t& used)
    {
        const WsServerOptions& o = shared_->srv;
        const std::size_t end = ws::FindHeadEnd(data, shared_->max_header_bytes, scan_from_);
        if (end == 0)
        {
            if (data.size() >= shared_->max_header_bytes) // 头已经超过上限还没结束
                return RejectUpgrade(431, data.size(), used);
            scan_from_ = static_cast<std::uint32_t>(data.size());
            return DecodeResult::NeedMore;
        }

        ws::HttpHead head;
        switch (ws::ParseHead(std::string_view(reinterpret_cast<const char*>(data.data()), end), head))
        {
            case ws::HeadParse::Ok:
                break;
            case ws::HeadParse::TooManyHeaders:
                return RejectUpgrade(431, data.size(), used);
            case ws::HeadParse::Malformed:
                return RejectUpgrade(400, data.size(), used);
        }

        const ws::ServerRequest req = ws::ClassifyUpgradeRequest(head, o);
        if (req.verdict == ws::RequestVerdict::Reject)
            return RejectUpgrade(req.status, data.size(), used);
        if (req.verdict == ws::RequestVerdict::Health)
        {
            const std::string resp = ws::BuildHealthResponse();
            host_->SendRaw(std::as_bytes(std::span(resp)));
            state_ = State::Dead;
            host_->CloseAfterFlush(CloseReason::LocalClose, 0); // 健康检查不是失败，不进握手失败统计
            used = data.size();
            return DecodeResult::Consumed;
        }

        const std::span<const std::string_view> offered(req.subprotocols.data(), req.subprotocol_count);
        WsUpgradeReply reply;
        if (o.on_upgrade)
        {
            const WsUpgradeRequest ur{.target = req.target,
                                      .path = req.path,
                                      .query = req.query,
                                      .host = req.host,
                                      .origin = req.origin,
                                      .subprotocols = offered,
                                      .headers = std::span<const WsHeader>(head.headers.data(), head.count),
                                      .peer = &host_->peer()};
            o.on_upgrade(ur, reply);
            if (reply.reject_status != 0)
            {
                int status = reply.reject_status;
                if (status < 400 || status > 599)
                {
                    LOG_ERROR("on_upgrade 给了不合法的拒绝状态码 {}（要在 400..599），按 500 处理", status);
                    status = 500;
                }
                return RejectUpgrade(status, data.size(), used);
            }
        }

        const std::string resp = ws::BuildUpgradeResponse(ws::ComputeAccept(req.key), ws::ChooseSubprotocol(o.subprotocols, offered));
        host_->SendRaw(std::as_bytes(std::span(resp)));
        if (reply.context)
            host_->SetContext(std::move(reply.context));
        // 要在 HandshakeDone 之前：它会回调 OnConnected，业务可能当场 CloseGracefully（例：拒绝被封的账号），
        // OnClosing 只有在 Open 时才会发 Close(1001)。（Send 不看 state_，不受影响。）
        // 守护用例：ws_server_test 的 TestCloseGracefullyInsideOnConnected。
        state_ = State::Open;
        used = end; // 紧跟在请求后面的字节（同一批到达的第一批帧）照常当帧解
        host_->HandshakeDone();
        return DecodeResult::Consumed;
    }

    // ---- 握手：客户端 ----

    void SendClientUpgrade()
    {
        const WsClientOptions& o = shared_->cli;
        if (!shared_->valid)
        {
            // 选项不安全（工厂已记过错误）：不发，让这条连接在握手期失败，别把含控制字符的东西写上线。
            state_ = State::Dead;
            host_->CloseAfterFlush(CloseReason::ProtocolError, 0);
            return;
        }
        std::string peer_text;
        std::string_view host_header = o.host;
        if (host_header.empty())
        {
            // 取对端地址，写成 Host 头的样子（host:port，IPv6 加方括号）。不能用 Endpoint::ToString：它带 "tcp://" 前缀。
            const auto& ep = host_->peer().endpoint;
            if (ep && ep->IsIp())
                peer_text = ep->IsIPv6() ? "[" + ep->address() + "]:" + std::to_string(ep->port())
                                         : ep->address() + ":" + std::to_string(ep->port());
            else
                peer_text = "localhost"; // UDS 上的 WebSocket：没有有意义的主机名，调用方应当填 WsClientOptions::host
            host_header = peer_text;
        }
        std::array<std::byte, 16> raw;
        const std::uint64_t a = ThreadRandom()();
        const std::uint64_t b = ThreadRandom()();
        std::memcpy(raw.data(), &a, 8);
        std::memcpy(raw.data() + 8, &b, 8);
        const std::string key = str::Base64Encode(raw);
        expected_accept_ = std::make_unique<std::string>(ws::ComputeAccept(key));
        const std::string req = ws::BuildUpgradeRequest(host_header, o.path, o.origin, o.subprotocols, key);
        host_->SendRaw(std::as_bytes(std::span(req)));
    }

    DecodeResult ClientHandshake(std::span<std::byte> data, std::size_t& used)
    {
        if (state_ == State::Dead || !expected_accept_) // 选项不安全，Attach 里已经关了
        {
            used = data.size();
            return DecodeResult::Consumed;
        }
        const std::size_t end = ws::FindHeadEnd(data, shared_->max_header_bytes, scan_from_);
        if (end == 0)
        {
            if (data.size() >= shared_->max_header_bytes)
                return DecodeResult::Error;
            scan_from_ = static_cast<std::uint32_t>(data.size());
            return DecodeResult::NeedMore;
        }
        ws::HttpHead head;
        if (ws::ParseHead(std::string_view(reinterpret_cast<const char*>(data.data()), end), head) != ws::HeadParse::Ok)
            return DecodeResult::Error;
        const ws::ResponseCheck check = ws::ValidateUpgradeResponse(head, *expected_accept_, shared_->cli.subprotocols);
        if (!check.ok)
        {
            LOG_DEBUG("ws upgrade response rejected, status={}", check.status);
            return DecodeResult::Error;
        }
        expected_accept_.reset();
        state_ = State::Open; // 同服务端：要在 HandshakeDone 之前（OnConnected 里业务可能当场 CloseGracefully）
        used = end;           // 101 之后紧跟的字节是服务端的第一批帧
        host_->HandshakeDone();
        return DecodeResult::Consumed;
    }

    // ---- 成员：每连接的常驻部分尽量小（10 万连接都带一份）----
    const std::shared_ptr<const ws::CodecShared> shared_;
    ICodecHost* host_ = nullptr;
    std::unique_ptr<Fragments> frag_;
    std::unique_ptr<std::string> expected_accept_; // 仅客户端握手期
    std::uint32_t scan_from_ = 0;                  // 握手期：已确认没有头部结束符的前缀长度
    std::array<std::byte, 4> mask_key_{};          // 仅客户端：EncodeHeader 生成、EncodePayload 使用的那把
    const bool server_;
    State state_ = State::Handshaking;
};

std::shared_ptr<ws::CodecShared> MakeShared(bool server)
{
    auto s = std::make_shared<ws::CodecShared>();
    s->server = server;
    return s;
}

} // namespace

WsServerCodecFactory::WsServerCodecFactory(WsServerOptions opts)
{
    auto s = MakeShared(true);
    s->type = opts.message_type;
    s->validate_utf8 = opts.validate_utf8;
    s->max_header_bytes = std::min(opts.max_header_bytes, kMaxHeaderCap);
    s->max_message_bytes = std::min(opts.max_message_bytes, kMaxMessageCap);
    s->srv = std::move(opts);
    shared_ = std::move(s);
}

WsServerCodecFactory::~WsServerCodecFactory() = default;

std::unique_ptr<IFrameCodec> WsServerCodecFactory::Create() const { return std::make_unique<WsCodec>(shared_); }

WsClientCodecFactory::WsClientCodecFactory(WsClientOptions opts)
{
    auto s = MakeShared(false);
    s->type = opts.message_type;
    s->validate_utf8 = opts.validate_utf8;
    s->max_header_bytes = std::min(opts.max_header_bytes, kMaxHeaderCap);
    s->max_message_bytes = std::min(opts.max_message_bytes, kMaxMessageCap);
    s->valid = ws::IsSafeTarget(opts.path) && ws::IsSafeHeaderValue(opts.host) && ws::IsSafeHeaderValue(opts.origin) &&
               std::ranges::all_of(opts.subprotocols, [](const std::string& p) { return ws::IsToken(p); });
    if (!s->valid)
        LOG_ERROR("WsClientOptions 含有不能写进升级请求的值（path 要以 '/' 开头且无空白；host / origin 不能含控制字符；子协议要是 token），"
                  "此后每条连接都会在握手期失败");
    s->cli = std::move(opts);
    shared_ = std::move(s);
}

WsClientCodecFactory::~WsClientCodecFactory() = default;

std::unique_ptr<IFrameCodec> WsClientCodecFactory::Create() const { return std::make_unique<WsCodec>(shared_); }

} // namespace breeze
