// 分层字节流连接：socket 与 codec 之间夹着一个 IStreamLayer（TLS），或 codec 要改写出站 payload（WebSocket 客户端，层是恒等层）。
// 读写路径与 PlainStreamConnection 同构，只是中间多了层：读 = read → layer.Unwrap → 分帧；
// 写 = 帧头 + payload 切成不超过一个 record 的块 → layer.Wrap → send 层里的待发字节。设计见方案 §3.5。
//
// **惰性加密**：复用基类的 out_ / Pending / offset 与 pending_bytes 的语义——pending_bytes 仍是"out_ 里尚未被加密的明文
// payload 字节"。无积压时直写：加密到预算（kWireBudget）为止直接 send；写不完的是密文，留在层里（有界，不计入
// pending_bytes），**不需要**把 payload 拷成 SharedBuffer；只有"本帧没被加密完"的剩余明文才入 out_（offset = 已加密字节数）。
// 积压时从 out_ 取若干帧合并进一个 record，再加密。所以广播的 payload 在背压下仍然是共享的，密文缓冲有界。
//
// 握手期间（层的握手没完成）连接不会 Wrap：codec 的 SendRaw（WebSocket 客户端的升级请求）先排进 out_，握手完成后再加密。
// 实现与基类分开放在 layered_stream_connection.cpp：慢路径不拖累明文热函数所在的编译单元。

#pragma once

#include <cstdint>
#include <memory>

#include "base/net/stream/internal/stream_connection.h"
#include "base/net/stream/stream_layer_interface.h"

namespace breeze
{

class LayeredStreamConnection final : public StreamConnection
{
public:
    SendResult Send(SharedBuffer payload) override;
    SendResult Send(std::span<const std::byte> payload) override;

private:
    friend class StreamConnection; // Create 里 new

    LayeredStreamConnection(EventLoop& loop,
                            UniqueFd fd,
                            PeerInfo peer,
                            IConnectionHandler& handler,
                            std::unique_ptr<IFrameCodec> codec,
                            const ConnectionOptions& options,
                            ConnectionTable::Slot& slot,
                            std::uint32_t slot_index,
                            ConnectionHandle handle,
                            std::unique_ptr<IStreamLayer> layer,
                            bool encodes_payload);

    void OnEvents(std::uint32_t events) override;
    void SendFromTask(SharedBuffer payload) override;
    void SendBatchFromTask(std::span<SharedBuffer> payloads) override;
    void OnEstablish() override;
    void FinishWrites() override;
    void WriteRaw(std::span<const std::byte> bytes) override;

    SendResult SendImpl(std::span<const std::byte> payload, SharedBuffer* owned, bool counted);
    void HandleRead();
    void HandleWrite();
    // 层交来的一块明文：与 PlainStreamConnection::HandleRead 里 read 之后的半帧逻辑相同。
    void OnPlainChunk(std::span<std::byte> plain);
    void HandleLayerStatus(LayerStatus st);
    // 把"帧头 + payload"从 offset 起依次加密进层，直到整帧加密完或层里待发字节达到预算；返回已加密的字节数（含 offset）。
    // header 可以为空（header_len = 0：原始字节）。层出错时关闭连接并返回。
    std::size_t EncryptFrame(const std::byte* header,
                             std::size_t header_len,
                             std::span<const std::byte> payload,
                             std::size_t offset);
    // 从 out_ 队首取若干帧（至多一个 record）加密，弹出已加密完的帧并记账；层里待发字节达到预算就停。
    void EncryptQueued();
    // 把层里的待发字节写进 socket；写不完就注册 EPOLLOUT。
    void FlushWire();
    // 关闭之前尽力写一次（层出错时的致命 alert、对端 close_notify 的回应）：写不出去就算了，不排队、不注册 EPOLLOUT。
    void FlushWireBestEffort();

    std::unique_ptr<IStreamLayer> layer_;
    const bool encodes_payload_;
    bool layer_shutdown_ = false; // 已让层发过关闭通知
};

} // namespace breeze
