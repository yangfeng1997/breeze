// 假的"一字节握手"codec（只给测试）：客户端在 Attach 里发 'H'，服务端收到后回 'W' 并完成握手；之后按 LengthPrefix 分帧。
// 握手之后的 "bye" 帧：codec 自己回一个原始字节 'B'，并要求写完后关闭，原因 PeerClosed、code 4001（模拟 WebSocket 的对端关闭）。
// 用来测握手阶段（OnConnected 延后、超时、失败、幂等、CloseAfterFlush 带出原因）与分层连接叠加 codec 握手，
// 不依赖任何 TLS / WebSocket。

#pragma once

#include <memory>
#include <span>
#include <string_view>

#include "base/net/codec/codec_host_interface.h"
#include "base/net/codec/length_prefix_codec.h"
#include "base/net/testing/net_test_util.h"

namespace breeze::testing
{

enum class HelloMode
{
    Normal,
    NeverDone, // 吞掉握手字节、从不应答、从不完成
    Reject,    // 服务端收到的第一个字节不是 'H'：回 'E'，CloseAfterFlush(ProtocolError, 400)
    DoneTwice, // 完成握手后再报告一次（幂等）
};

inline std::span<const std::byte> RawBytes(std::string_view s) { return std::as_bytes(std::span(s)); }

class HelloCodec final : public IFrameCodec
{
public:
    static constexpr std::size_t kMaxFrame = 1 << 20;

    HelloCodec(bool server, HelloMode mode)
        : server_(server)
        , mode_(mode)
        , inner_(kMaxFrame)
    {
    }

    CodecTraits traits() const noexcept override { return {.needs_handshake = true}; }

    void Attach(ICodecHost& host) override
    {
        host_ = &host;
        if (!server_)
            host.SendRaw(RawBytes("H"));
    }

    void OnClosing() override { host_->SendRaw(RawBytes("B")); }

    DecodeResult Decode(std::span<std::byte> data, FrameSink sink, std::size_t& used) override
    {
        used = 0;
        if (!done_)
        {
            if (data.empty())
                return DecodeResult::NeedMore;
            if (mode_ == HelloMode::NeverDone)
            {
                used = data.size();
                return DecodeResult::Consumed;
            }
            const char c = static_cast<char>(data[0]);
            if (server_ && c != 'H')
            {
                if (mode_ != HelloMode::Reject)
                    return DecodeResult::Error;
                host_->SendRaw(RawBytes("E"));
                host_->CloseAfterFlush(CloseReason::ProtocolError, 400);
                used = data.size();
                return DecodeResult::Consumed;
            }
            if (!server_ && c != 'W')
                return DecodeResult::Error;
            used = 1;
            done_ = true;
            if (server_)
                host_->SendRaw(RawBytes("W"));
            host_->HandshakeDone();
            if (mode_ == HelloMode::DoneTwice)
                host_->HandshakeDone();
            return DecodeResult::Consumed;
        }

        bool bye = false;
        const DecodeResult r = inner_.Decode(
            data,
            [&](std::span<const std::byte> p)
            {
                if (Str(p) == "bye")
                    bye = true;
                else
                    sink(p);
            },
            used);
        if (r == DecodeResult::Ok && bye)
        {
            host_->SendRaw(RawBytes("B"));
            host_->CloseAfterFlush(CloseReason::PeerClosed, 4001);
            return DecodeResult::Consumed;
        }
        return r;
    }

    std::size_t EncodeHeader(std::size_t payload_len, std::span<std::byte, kMaxFrameHeaderSize> header_out) override
    {
        return inner_.EncodeHeader(payload_len, header_out);
    }

private:
    const bool server_;
    const HelloMode mode_;
    LengthPrefixCodec inner_;
    ICodecHost* host_ = nullptr;
    bool done_ = false;
};

class HelloCodecFactory final : public IFrameCodecFactory
{
public:
    HelloCodecFactory(bool server, HelloMode mode)
        : server_(server)
        , mode_(mode)
    {
    }
    std::unique_ptr<IFrameCodec> Create() const override { return std::make_unique<HelloCodec>(server_, mode_); }

private:
    bool server_;
    HelloMode mode_;
};

} // namespace breeze::testing
