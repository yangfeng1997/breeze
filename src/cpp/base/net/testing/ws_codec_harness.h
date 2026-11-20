// WebSocket codec 的测试 / fuzz 共用件（只给 *_test.cpp 与 fuzz 目标用，不进库）：假宿主 WsFakeHost 与模拟连接解码循环的 WsPeer。
// WsPeer::Feed 照连接的样子解码（半个帧留在缓冲里、Consumed 推进、Error 即停），并检查契约：NeedMore / Error 不得改动缓冲、
// Consumed 必须推进、不能原地空转；违约直接 abort（单元测试失败、fuzz 报崩溃）。

#pragma once

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/net/codec/codec_host_interface.h"
#include "base/net/codec/frame_codec_interface.h"
#include "base/net/testing/net_test_util.h"

namespace breeze::testing
{

struct WsHostClosed
{
    CloseReason reason;
    int code;
};

class WsFakeHost final : public ICodecHost
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
            closed = WsHostClosed{r, c};
        events.push_back("X");
    }
    void SetContext(std::unique_ptr<ConnectionContext> c) override
    {
        ctx = std::move(c);
        events.push_back("C");
    }
    const PeerInfo& peer() const noexcept override { return peer_; }

    std::string sent; // SendRaw 写出的全部字节
    std::vector<std::string> events;      // S = 一次 SendRaw、H = HandshakeDone、X = CloseAfterFlush、C = SetContext、M = 一条消息
    int handshake_done = 0;
    int close_calls = 0;
    std::optional<WsHostClosed> closed;
    std::unique_ptr<ConnectionContext> ctx;
    PeerInfo peer_;
};

struct WsPeer
{
    explicit WsPeer(const IFrameCodecFactory& f, std::optional<Endpoint> ep = std::nullopt)
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

    WsFakeHost host;
    std::unique_ptr<IFrameCodec> codec;
    std::vector<std::byte> buf; // 还没解完的字节
    std::vector<std::string> messages;
    bool error = false; // Decode 返回过 Error
};

} // namespace breeze::testing
