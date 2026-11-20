// 恒等层：不改任何字节、没有握手。给"没有 TLS、但 codec 要改写出站 payload"的连接用（WebSocket 客户端走明文时），
// 让它们仍然走分层连接（LayeredStreamConnection）这一条慢写路径，而明文连接（PlainStreamConnection）保持零拷贝快路径。
// 代价是每条出站消息多一次拷贝；这样的连接是机器人 / 压测 / 服务间的客户端，不是网关的热路径。

#pragma once

#include "base/buffer/buffer.h"
#include "base/net/stream/stream_layer_interface.h"

namespace breeze
{

class IdentityLayer final : public IStreamLayer
{
public:
    LayerStatus Start() override { return LayerStatus::HandshakeDone; }

    LayerStatus Unwrap(std::span<std::byte> wire, PlainSink sink) override
    {
        if (!wire.empty())
            (void)sink(wire);
        return LayerStatus::Ok;
    }

    LayerStatus Wrap(std::span<const std::byte> plain) override
    {
        wire_.Append(plain);
        return LayerStatus::Ok;
    }

    std::span<const std::byte> PendingWire() const noexcept override { return wire_.Readable(); }

    void ConsumeWire(std::size_t n) noexcept override
    {
        wire_.Consume(n);
        if (wire_.ReadableBytes() == 0 && wire_.Capacity() > kShrinkAbove)
            wire_.ShrinkIfEmpty(0);
    }

    LayerStatus Shutdown() override { return LayerStatus::Ok; }
    int error_code() const noexcept override { return 0; }

private:
    static constexpr std::size_t kShrinkAbove = 64 * 1024;
    Buffer wire_;
};

} // namespace breeze
