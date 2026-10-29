// 一条已建立的连接，TCP 与 UDS 同一个类：差异都收在建立时（见 socket.h），读写热路径里没有 if tcp。
// 设计见 docs/网络库设计.md §4.4 / §4.6 / §4.7 / §4.8。
//
// 只能在所属 EventLoop 的 IO 线程上使用（回调里拿到的 Connection& 即是）。

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>

#include "buffer.h"
#include "connection_handler.h"
#include "frame_codec.h"
#include "internal/pollable.h"
#include "internal/socket.h"
#include "peer_info.h"
#include "shared_buffer.h"

namespace breeze
{

class EventLoop;

enum class SendResult : std::uint8_t
{
    Ok,     // 网络层已接收，不表示送达
    Closed, // 连接已关闭或正在关闭，原因见 OnClosed
};

class Connection final : private Pollable
{
public:
    Connection(EventLoop& loop, Fd fd, PeerInfo peer, ConnectionHandler& handler, std::unique_ptr<FrameCodec> codec);
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // 发送一帧：帧头由本连接的 codec 生成，payload 不拷贝（同一块可以同时发给多条连接）。
    // IO 线程内写队列为空时先直接写，写不完才入队并注册 EPOLLOUT；已有积压则排在后面，保序。
    SendResult Send(SharedBuffer payload);

    // 立即关闭。连接上不再有读写，OnClosed 在本轮分发之后回调（不在当前调用栈里），对象此后才销毁。
    // 重复调用、对已关闭的连接调用都是空操作。
    void Close();

    bool IsConnected() const noexcept { return state_ == State::Connected; }
    const PeerInfo& peer() const noexcept { return peer_; }

    // 装配代码（LoopServer）在把连接交给 loop 持有之后调用：登记到 epoll 并回调 OnConnected。
    // 登记失败（新 fd 的 epoll_ctl 遇到 ENOSPC / ENOMEM）返回 false：只关这一条，没有任何回调，进程继续。
    [[nodiscard]] bool Establish();

private:
    friend class EventLoop;

    enum class State : std::uint8_t
    {
        Connected,
        Disconnected,
    };

    // 待发送的一帧：内联帧头 + 共享的 payload；offset 是已经写出的字节数（帧头 + payload 合计）。
    struct Pending
    {
        std::array<std::byte, kMaxFrameHeaderSize> header{};
        std::uint8_t header_len = 0;
        SharedBuffer payload;
        std::size_t offset = 0;

        std::size_t Total() const noexcept { return header_len + payload.size(); }
    };

    void OnEvents(std::uint32_t events) override;
    void HandleRead();
    std::size_t DecodeAll(std::span<const std::byte> data);
    void HandleWrite();
    void SetWriteInterest(bool want);
    void CloseWith(CloseReason reason, int sys_errno);
    void NotifyClosed(); // 只由 EventLoop 在回收时调用

    EventLoop& loop_;
    Fd fd_;
    PeerInfo peer_;
    ConnectionHandler& handler_;
    std::unique_ptr<FrameCodec> codec_;

    Buffer in_;
    std::deque<Pending> out_;

    State state_ = State::Connected;
    bool registered_ = false;         // 是否已登记到 epoll
    bool want_write_ = false;         // epoll 里是否带 EPOLLOUT
    bool notified_connected_ = false; // 回调过 OnConnected 才会回调 OnClosed
    CloseReason close_reason_ = CloseReason::LocalClose;
    int close_errno_ = 0;
};

} // namespace breeze
