// 字节流连接（TCP 与 UDS 同一个类）：Connection 接口的实现。差异都收在建立时（见 socket.h），读写热路径里没有 if tcp。
// 设计见 docs/网络库设计.md §4.4 / §4.6 / §4.7 / §4.8。
//
// 内部类型：业务回调里拿到的是 Connection&。创建只经 Create（EventLoop 不认识它：core 不依赖 stream）。
//
// 成员顺序是性能决定的（见下面成员处的注释）：Connection 接口没有数据成员、继承链上只有一个 vptr，
// 所以布局与抽象之前一致。

#pragma once

#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <type_traits>

#include "buffer.h"
#include "connection.h"
#include "connection_context.h"
#include "connection_handle.h"
#include "connection_handler.h"
#include "connection_options.h"
#include "frame_codec.h"
#include "internal/core/connection_table.h"
#include "internal/core/unique_fd.h"
#include "peer_info.h"
#include "send_result.h"
#include "shared_buffer.h"

namespace breeze
{

class EventLoop;

class StreamConnection final : public Connection
{
public:
    // 在 loop 的连接表里占一个槽并建出连接（归 loop 持有，回收时销毁），调用方随后调 Establish()。
    // 调用方须在 loop 线程上，且已 table().TryReserve() 成功；
    // codec 由调用方（在本 loop 线程上）从 FrameCodecFactory 创建。
    static StreamConnection* Create(EventLoop& loop,
                                    UniqueFd fd,
                                    PeerInfo peer,
                                    ConnectionHandler& handler,
                                    std::unique_ptr<FrameCodec> codec,
                                    const ConnectionOptions& options);

    ~StreamConnection() override;

    // 语义见 Connection 接口。
    SendResult Send(SharedBuffer payload) override;
    SendResult Send(std::span<const std::byte> payload) override;
    void Close() override;
    void CloseGracefully(std::chrono::milliseconds timeout) override;
    void PauseRead() override;
    void ResumeRead() override;
    void SetContext(std::unique_ptr<ConnectionContext> ctx) noexcept override { ctx_ = std::move(ctx); }
    bool IsConnected() const noexcept override { return state_ == State::Connected; }
    bool IsPaused() const noexcept override { return paused_; }
    const PeerInfo& peer() const noexcept override { return peer_; }
    ConnectionHandle handle() const noexcept override { return handle_; }

    // 装配代码（StreamServer / StreamClient）在 Create 之后调用：登记到 epoll 并回调 OnConnected。
    // 登记失败（新 fd 的 epoll_ctl 遇到 ENOSPC / ENOMEM）返回 false：只关这一条，没有任何回调，进程继续。
    [[nodiscard]] bool Establish();

private:
    StreamConnection(EventLoop& loop,
                     UniqueFd fd,
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
        Disconnecting, // 优雅关闭中：不再接受 Send，仍读，等写队列排空与对端关闭
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
    // 以下由 EventLoop 经 Connection 接口调用。
    ConnectionContext* context() const noexcept override { return ctx_.get(); }
    void SendFromTask(SharedBuffer payload) override;
    void CloseFromTask(CloseReason reason) override { CloseWith(reason, 0); }
    void CloseIdle() override { CloseWith(CloseReason::IdleTimeout, 0); }
    void NotifyClosed() override;
    std::uint32_t slot_index() const noexcept override { return slot_index_; }
    void HandleRead();
    std::size_t DecodeAll(std::span<const std::byte> data);
    void HandleWrite();
    void SetWriteInterest(bool want);
    void ApplyInterest();
    void ShutdownWrite();
    // 对端关闭（EOF / HUP）的原因：我们先发起了优雅关闭时，对端回应的 FIN 是正常结束，算 LocalClose。
    CloseReason PeerCloseReason() const noexcept
    {
        return state_ == State::Disconnecting ? CloseReason::LocalClose : CloseReason::PeerClosed;
    }
    void CloseWith(CloseReason reason, int sys_errno);

    // 成员顺序是性能决定的：每条消息都碰的热字段集中在前（两条缓存行内），冷的（对端信息、句柄、上下文、定时器、关闭原因）放后面。
    // 10 万连接时每连接多碰一条缓存行就是可测量的代价（docs/网络库设计.md §9.1 / §9.3）。改动时用 sizeof 与 offsetof
    // 核对。
    EventLoop& loop_;
    UniqueFd fd_;
    ConnectionHandler& handler_;
    std::unique_ptr<FrameCodec> codec_;
    ConnectionTable::Slot& slot_; // 热字段（状态、pending_bytes）在连接表里，别的线程不碰本对象就能读
    const std::uint32_t slot_index_;
    const std::uint32_t idle_timeout_ms_; // 0 = 不参加空闲检测
    const std::uint64_t high_water_;
    State state_ = State::Connected;
    bool registered_ = false;         // 是否已登记到 epoll
    bool want_write_ = false;         // epoll 里是否带 EPOLLOUT
    bool paused_ = false;             // PauseRead 生效中（epoll 里不带 EPOLLIN）
    bool idle_registered_ = false;    // 在 EventLoop 的空闲检测里登记过
    bool notified_connected_ = false; // 回调过 OnConnected 才会回调 OnClosed
    Buffer in_;
    std::deque<Pending> out_;

    PeerInfo peer_;
    const ConnectionHandle handle_;
    std::unique_ptr<ConnectionContext> ctx_;
    std::uint64_t grace_timer_ = 0; // CloseGracefully 的超时定时器（EventLoop::TimerId），0 = 没有
    CloseReason close_reason_ = CloseReason::LocalClose;
    int close_errno_ = 0;
};

} // namespace breeze
