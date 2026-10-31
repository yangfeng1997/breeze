// rudp 会话：Connection 接口的实现，KCP 之上的一条可靠有序的消息通道。设计见 docs/网络库设计.md §4.14。
//
// 没有自己的 fd：服务端所有会话共用一个 UDP socket，由服务端按来源地址把数据报分给会话；客户端的 socket 归客户端。
// 所以 OnEvents 不会被调用。会话不自己起定时器：拥有者（服务端 / 客户端）每个 interval 调一次 Tick。
// 静止的会话（没有未确认的数据、没有待发的确认、对端窗口不为 0，见 KcpSession::NeedsUpdate）不需要 Tick：
// Tick 返回 false 就是"暂时不用再调我"，之后 Input / Send / ResumeRead 打破静止时通过 Owner::OnConnectionWake
// 把它重新放回拥有者的活跃表。会话很多、大多空闲时，定时器只走活跃的那部分，空闲会话不占 CPU。
// 活跃的会话也不是每拍都真调 Update：只有在途的数据时，要等到最早的重传时刻（KcpSession::PlanUpdate），
// Tick 没到点就直接返回（仍在表里）；有待发的确认 / 排队的数据 / 新来的 Input、Send 才下一拍就推进。
// 往返时间长（几十毫秒以上）时，每条消息的在途时间里会话都在活跃表里，这一步才省得下东西。
//
// 与 StreamConnection 的语义差异（都写在 Connection 接口的注释之外，因为是 rudp 特有的）：
// - 消息自带边界，没有 codec；单条消息上限是 RudpOptions::max_message_size，
//   超限的 Send 与 TCP 的 codec 超限一样关连接。
// - PauseRead：停止向 handler 交付，消息留在 KCP 的接收队列里，队列占满接收窗口后对端自然被拖慢（真背压，不丢）。
//   ResumeRead 后在下一次 Tick（≤ interval_ms）里补交付，不在调用栈里直接交付：调用方可能就在 OnMessage 里。
// - CloseGracefully：之后的 Send 返回 Closed；已排队的消息被对端全部确认后才发 Close 并结束，timeout 到了强关。
//   原因一律是 LocalClose。对端收到 Close 以 PeerClosed 结束。
// - 发送积压（pending_bytes）按"已排进 KCP、但整条消息还没进发送窗口的字节"算，与 TCP 只算"内核缓冲放不下的用户态排队"
//   同一个边界：对端慢了，窗口关上，队列才会涨，越过水位就是慢消费者。已进窗口、等确认的部分由 send_window 限界，
//   不算积压（否则 RTT 内的在途数据就会让健康对端上的突发越过水位）。队列先进先出，按累计段数精确释放。
//   KCP 只在 flush 时才把队列移进窗口，所以排队量超过水位一半时 Send 会立刻 flush 一次再判水位；
//   跨线程的 ConnectionHandle::Send 在句柄侧判水位、不能 flush：同一个 interval 内从别的线程塞进的量超过水位仍会越线，
//   突发大的业务调大 high_water_mark（或开 flush_on_send）。
// - Close / 超限 / 空闲 / 慢消费者关闭时向对端发 3 个 Close 包（尽力而为，丢了对端靠心跳超时）；
//   对端发来 Close 引起的关闭、链路判死（IoError）不再发。

#pragma once

#include <sys/socket.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>

#include "connection.h"
#include "connection_options.h"
#include "internal/core/connection_table.h"
#include "kcp_session.h"
#include "peer_info.h"
#include "rudp_options.h"
#include "rudp_protocol.h"
#include "udp_socket.h"

namespace breeze
{

class EventLoop;

class RudpConnection final : public Connection
{
public:
    // 会话的拥有者（服务端 / 客户端）。
    class Owner
    {
    public:
        // 会话回收前、OnClosed 之前调用：把它从自己的会话表里摘掉。只会调用一次，且不在 Tick 的遍历里。
        // 此时会话若还在活跃表里（ticking() 为真），拥有者要一并摘掉。
        virtual void OnConnectionDetached(RudpConnection& conn) noexcept = 0;
        // 静止的会话被唤醒（有了要 Tick 的事）：拥有者把它放进活跃表并保证定时器在跑。只在 ticking() 由假变真时调一次。
        virtual void OnConnectionWake(RudpConnection& conn) noexcept = 0;

    protected:
        ~Owner() = default;
    };

    // 在 loop 的连接表里占一个槽并建出会话（归 loop 持有，回收时销毁），调用方随后调 Establish()。
    // 调用方须在 loop 线程上，且已 table().TryReserve() 成功。KCP 创建失败（内存不足）返回空，此时没有占槽，
    // 调用方自己归还名额。peer_addr 为空 = socket 已 connect（客户端），否则是服务端收包时的来源地址。
    // socket / handler / owner 必须比本会话活得久（回收之前）。
    static RudpConnection* Create(EventLoop& loop,
                                  UdpSocket& socket,
                                  const sockaddr_storage* peer_addr,
                                  socklen_t peer_len,
                                  std::uint32_t conv,
                                  PeerInfo peer,
                                  ConnectionHandler& handler,
                                  Owner& owner,
                                  const ConnectionOptions& conn_options,
                                  const RudpOptions& rudp_options);

    ~RudpConnection() override;

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

    // 登记空闲检测、初始化 KCP 的时钟并回调 OnConnected。
    void Establish();

    // 拥有者把属于本会话的数据报交过来（已按来源 / conv 找到会话，Hello / Accept / Reject 不会到这里）。
    void OnPacket(const rudp::Packet& packet);

    // 活跃表里的会话每个 interval 调一次：补交付、推进 KCP、释放已进窗口的积压、判链路死、优雅关闭收尾。
    // 返回是否还需要继续 Tick；false 时 ticking() 已变假，拥有者把它从活跃表摘掉。
    bool Tick(std::uint32_t now_ms);
    // 在拥有者的活跃表里（拥有者据此在摘表时一并摘掉）。
    bool ticking() const noexcept { return ticking_; }

    std::uint32_t conv() const noexcept { return conv_; }
    bool closed() const noexcept { return state_ == State::Disconnected; }

    // 拥有者记在它的会话数组 / 活跃表里的下标，摘除时 O(1)；本类不读它。
    std::size_t owner_index = 0;
    std::size_t tick_index = 0;

private:
    enum class State : std::uint8_t
    {
        Connected,
        Disconnecting, // 优雅关闭中：不再接受 Send，等已排队的消息被确认
        Disconnected,
    };

    // 一条已排进 KCP 发送队列的消息：它的最后一段在"累计排入段数"里的位置，与它占的字节（已计入槽的 pending_bytes）。
    struct Queued
    {
        std::uint64_t end_seg;
        std::uint32_t bytes;
    };

    RudpConnection(EventLoop& loop,
                   UdpSocket& socket,
                   const sockaddr_storage* peer_addr,
                   socklen_t peer_len,
                   std::uint32_t conv,
                   std::unique_ptr<KcpSession> kcp,
                   PeerInfo peer,
                   ConnectionHandler& handler,
                   Owner& owner,
                   const ConnectionOptions& conn_options,
                   const RudpOptions& rudp_options,
                   ConnectionTable::Slot& slot,
                   std::uint32_t slot_index,
                   ConnectionHandle handle);

    void OnEvents(std::uint32_t) override; // 没有自己的 fd：不会被调用
    ConnectionContext* context() const noexcept override { return ctx_.get(); }
    void SendFromTask(SharedBuffer payload) override;
    void CloseFromTask(CloseReason reason) override { CloseWith(reason, 0); }
    void CloseIdle() override { CloseWith(CloseReason::IdleTimeout, 0); }
    void NotifyClosed() override;
    std::uint32_t slot_index() const noexcept override { return slot_index_; }

    // counted：发送方（跨线程的 ConnectionHandle::Send）已经把 payload.size() 计入 pending_bytes，并做过水位检查。
    SendResult SendImpl(std::span<const std::byte> payload, bool counted);
    void DeliverReady();
    void ReleaseSent();
    void Wake();
    void CloseWith(CloseReason reason, int sys_errno);
    void SendControl(const rudp::Control& c) noexcept;
    static void KcpOutput(void* user, std::span<const std::byte> segment);
    CloseReason PeerCloseReason() const noexcept
    {
        return state_ == State::Disconnecting ? CloseReason::LocalClose : CloseReason::PeerClosed;
    }

    EventLoop& loop_;
    UdpSocket& socket_;
    ConnectionHandler& handler_;
    Owner& owner_;
    std::unique_ptr<KcpSession> kcp_;
    ConnectionTable::Slot& slot_;
    const std::uint32_t slot_index_;
    const std::uint32_t conv_;
    const std::uint32_t idle_timeout_ms_;
    const std::uint64_t high_water_;
    const std::size_t max_message_;
    const bool flush_on_send_;
    State state_ = State::Connected;
    bool paused_ = false;
    bool need_deliver_ = false; // ResumeRead 之后等 Tick 补交付
    bool idle_registered_ = false;
    bool ticking_ = false;            // 在拥有者的活跃表里
    bool immediate_ = true;           // 下一拍就要推进（有新的 Input / Send、有待发的确认……），不看 due_ms_
    std::uint32_t due_ms_ = 0;        // 否则要等到这个时刻（KCP 的毫秒时钟）再推进：最早的重传 / 探测时刻
    bool notified_connected_ = false; // 回调过 OnConnected 才会回调 OnClosed

    std::uint64_t seg_total_ = 0; // 累计排入 KCP 的段数
    std::deque<Queued> queued_;   // 还没整条进窗口的消息，按排入顺序

    sockaddr_storage peer_addr_{};
    socklen_t peer_len_ = 0; // 0 = socket 已 connect，发包不带地址

    PeerInfo peer_;
    const ConnectionHandle handle_;
    std::unique_ptr<ConnectionContext> ctx_;
    std::uint64_t grace_timer_ = 0; // CloseGracefully 的超时定时器（EventLoop::TimerId），0 = 没有
    CloseReason close_reason_ = CloseReason::LocalClose;
    int close_errno_ = 0;
};

} // namespace breeze
