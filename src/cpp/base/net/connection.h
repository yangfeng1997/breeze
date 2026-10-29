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
#include "connection_handle.h"
#include "connection_handler.h"
#include "connection_options.h"
#include "frame_codec.h"
#include "internal/connection_table.h"
#include "internal/pollable.h"
#include "internal/socket.h"
#include "peer_info.h"
#include "send_result.h"
#include "shared_buffer.h"

namespace breeze
{

class EventLoop;

class Connection final : private Pollable
{
public:
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // 发送一帧：帧头由本连接的 codec 生成，payload 不拷贝（同一块可以同时发给多条连接）。
    // IO 线程内写队列为空时先直接写，写不完才入队并注册 EPOLLOUT；已有积压则排在后面，保序。
    SendResult Send(SharedBuffer payload);

    // 视图版本：不拿走所有权，也不要求调用方先拷成 SharedBuffer。尽量直接从 payload
    // 写出（写队列为空时整帧写完就一次拷贝都没有）， 写不完的部分（或前面已有积压、必须排队保序时）才把 payload 拷成
    // SharedBuffer 入队——所以返回之后 payload 指向的内存可以立刻改写或释放。 给在 IO 线程里立即回包的 handler
    // 用（echo、小的应答、栈上拼出来的响应）：OnMessage 的 payload 只在回调内有效， 以前只能先 CopyFrom 再
    // Send，每条消息白白多一次分配与拷贝。已经有 SharedBuffer（要跨连接共享、要入队）时用上面那个。
    SendResult Send(std::span<const std::byte> payload);

    // 立即关闭。连接上不再有读写，OnClosed 在本轮分发之后回调（不在当前调用栈里），对象此后才销毁。
    // 重复调用、对已关闭的连接调用都是空操作。
    void Close();

    bool IsConnected() const noexcept { return state_ == State::Connected; }
    const PeerInfo& peer() const noexcept { return peer_; }

    // 可复制、可跨线程使用的句柄（ConnectionHandle::Send / Close）。独立 loop（不属于任何 group）上的连接没有句柄，
    // 此时返回无效句柄（raw == 0）。
    ConnectionHandle handle() const noexcept { return handle_; }

    // 装配代码（NetServer / NetClient）在 EventLoop::NewConnection 之后调用：登记到 epoll 并回调 OnConnected。
    // 登记失败（新 fd 的 epoll_ctl 遇到 ENOSPC / ENOMEM）返回 false：只关这一条，没有任何回调，进程继续。
    [[nodiscard]] bool Establish();

private:
    friend class EventLoop; // 唯一的创建者与任务执行者

    Connection(EventLoop& loop,
               Fd fd,
               PeerInfo peer,
               ConnectionHandler& handler,
               std::unique_ptr<FrameCodec> codec,
               const ConnectionOptions& options,
               ConnectionTable::Slot& slot,
               std::uint32_t slot_index,
               ConnectionHandle handle);

    enum class State : std::uint8_t
    {
        Connected,
        Disconnected,
    };

    // 待发送的一帧：内联帧头 + 共享的 payload；offset 是已经写出的字节数（帧头 + payload 合计）。
    // 队列里每一项的 payload.size() 都已计入槽的 pending_bytes，整条写完（或连接回收）时才减。
    struct Pending
    {
        std::array<std::byte, kMaxFrameHeaderSize> header{};
        std::uint8_t header_len = 0;
        SharedBuffer payload;
        std::size_t offset = 0;

        std::size_t Total() const noexcept { return header_len + payload.size(); }
    };

    void OnEvents(std::uint32_t events) override;
    // counted：发送方（跨线程的 ConnectionHandle::Send）已经把 payload.size() 计入 pending_bytes，并做过水位检查。
    SendResult SendImpl(std::span<const std::byte> payload, SharedBuffer* owned, bool counted);
    // 只由 EventLoop 在执行跨线程任务时调用。
    void SendFromTask(SharedBuffer payload);
    void CloseFromTask(CloseReason reason) { CloseWith(reason, 0); }
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
    ConnectionTable::Slot& slot_; // 热字段（状态、pending_bytes）在连接表里，别的线程不碰本对象就能读
    const std::uint32_t slot_index_;
    const ConnectionHandle handle_;
    const std::uint64_t high_water_;

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
