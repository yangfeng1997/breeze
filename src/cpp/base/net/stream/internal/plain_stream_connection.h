// 明文字节流连接：fd 直接读写。读 / 写 / 分帧的热路径原样来自拆分之前的 StreamConnection，函数体没有改动。
// 设计见 docs/网络库设计.md §4.4 / §4.6 / §4.7。
//
// 实现与基类同在 stream_connection.cpp（不是 plain_stream_connection.cpp）：热函数调用的共用小函数
// （DecodeAll / SetWriteInterest / ShutdownWrite …）要和它们在同一个编译单元里，才能保持拆分之前的内联。

#pragma once

#include "base/net/stream/internal/stream_connection.h"

namespace breeze
{

class PlainStreamConnection final : public StreamConnection
{
public:
    SendResult Send(SharedBuffer payload) override;
    SendResult Send(std::span<const std::byte> payload) override;

private:
    friend class StreamConnection; // Create 里 new

    PlainStreamConnection(EventLoop& loop,
                          UniqueFd fd,
                          PeerInfo peer,
                          IConnectionHandler& handler,
                          std::unique_ptr<IFrameCodec> codec,
                          const ConnectionOptions& options,
                          ConnectionTable::Slot& slot,
                          std::uint32_t slot_index,
                          ConnectionHandle handle);

    void OnEvents(std::uint32_t events) override;
    void SendFromTask(SharedBuffer payload) override;
    void SendBatchFromTask(std::span<SharedBuffer> payloads) override;
    // ICodecHost::SendRaw 的实现（握手应答、控制帧）：冷路径，单独成函数，不碰 SendImpl 的函数体。
    void WriteRaw(std::span<const std::byte> bytes) override;
    // counted：发送方（跨线程的 ConnectionHandle::Send）已经把 payload.size() 计入 pending_bytes，并做过水位检查。
    SendResult SendImpl(std::span<const std::byte> payload, SharedBuffer* owned, bool counted);
    void HandleRead();
    void HandleWrite();
};

// 工厂按后端选定，避免 epoll 的 Send / HandleRead 为 completion 增加每消息分支。
class CompletionPlainStreamConnection final : public StreamConnection
{
public:
    SendResult Send(SharedBuffer payload) override;
    SendResult Send(std::span<const std::byte> payload) override;

private:
    friend class StreamConnection;

    CompletionPlainStreamConnection(EventLoop& loop,
                                    UniqueFd fd,
                                    PeerInfo peer,
                                    IConnectionHandler& handler,
                                    std::unique_ptr<IFrameCodec> codec,
                                    const ConnectionOptions& options,
                                    ConnectionTable::Slot& slot,
                                    std::uint32_t slot_index,
                                    ConnectionHandle handle)
        : StreamConnection(loop, std::move(fd), std::move(peer), handler, std::move(codec), options, slot, slot_index, handle)
    {
    }

    void OnEvents(std::uint32_t) override {}
    void SendFromTask(SharedBuffer payload) override;
    void SendBatchFromTask(std::span<SharedBuffer> payloads) override;
    void FinishWrites() override;
    void WriteRaw(std::span<const std::byte> bytes) override;
    void OnIoRead(std::span<std::byte> data, int error) override;
    void OnIoWrite(std::size_t bytes, int error) override;
    SendResult Enqueue(std::span<const std::byte> payload, SharedBuffer* owned, bool counted, bool raw);
    void SubmitQueued();
    void CloseAfterIoPrefix(CloseReason reason);
    bool HasPendingFirst() const noexcept { return pending_header_len_ != 0 || !pending_wire_.empty(); }
    // 视图首帧存完整 wire；owned 首帧只留 payload 引用与独立帧头，终结 CQE 前不能改写（SubmitQueued）。
    SharedBuffer pending_wire_;
    std::size_t pending_payload_bytes_ = 0;
    std::size_t pending_wire_offset_ = 0;
    CloseReason io_close_reason_ = CloseReason::LocalClose;
    std::array<std::byte, kMaxFrameHeaderSize> pending_header_{};
    std::uint8_t pending_header_len_ = 0; // 非零也表示空 payload 的帧头仍待发送
};

} // namespace breeze
