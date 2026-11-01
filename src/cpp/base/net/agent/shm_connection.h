// 共享内存通道做成的 IConnection（App 一侧，内部类型）：业务回调里拿到的是 IConnection&，与 TCP / UDS 的连接用法一样，
// 经 ConnectionHandle 跨线程发送、MainLoopExecutor 回投主循环都照旧可用。创建只经 Create，由 AgentClient 持有与驱动。
// 设计见 docs/共享内存IPC设计.md §5 / §6。
//
// 它和 StreamConnection 的区别：
// - 数据不走 fd：Tx 环（App → Agent）的生产者与 Rx 环（Agent → App）的消费者都在本连接所属的 IO 线程上
//   （SPSC 要求各只有一个线程；业务线程发送经 ConnectionHandle 投递到这个线程）。
// - **Agent 不在了连接不关**：UDS 断开由 AgentClient 发现并去 Reattach，期间 Send 照常写 Tx 环、Rx 里剩的照常读完；
//   环满了这条丢掉、计入 stats().tx_full_drops，**IO 线程上直接 Send 得到 SendResult::Full**；
//   业务线程经 ConnectionHandle::Send 时写环在 IO 线程上异步发生，调用方当场看不到：环满时置槽的 tx_full 提示，
//   kFullHintWindow 之内的后续发送当场得到 Full（不入队），提示清掉之后的第一条才真的去试。
//   所以 Full 保证"没发出"，Ok 不保证"发出了"（提示置位之前已投递的那几条仍可能随后被丢，只进 tx_full_drops）。
// - 没有空闲检测、没有写队列水位（写环是同步的，不排队）。
// - PauseRead 暂停读 Rx 环（留在环里，不丢）；ResumeRead 接着读。

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

#include "base/net/connection/connection_context.h"
#include "base/net/connection/connection_handler_interface.h"
#include "base/net/connection/connection_interface.h"
#include "base/net/connection/connection_options.h"
#include "base/net/endpoint/peer_info.h"
#include "base/net/loop/connection_table.h"
#include "base/shm/shm_channel.h"
#include "base/shm/shm_ring.h"

namespace breeze::agent
{

// 连接的计数，任意线程可读。由 shared_ptr 持有，连接销毁之后还能读。
struct ShmConnStats
{
    std::atomic<std::uint64_t> tx_messages{0};   // 写进 Tx 环的消息数
    std::atomic<std::uint64_t> tx_full_drops{0}; // 写 Tx 环时遇满被丢的（跨线程被 tx_full 提示当场拒绝的不在内，见 NetStats::send_full）
    std::atomic<std::uint64_t> tx_too_large{0};  // 超过 Tx 环单条上限被丢的
    std::atomic<std::uint64_t> rx_messages{0};   // 从 Rx 环读出并交给业务的消息数
};

class ShmConnection final : public IConnection
{
public:
    // Tx 环遇满之后，跨线程发送直接得到 Full 的时长：太长会把 Agent 刚腾出的空间白白让掉，太短等于没有提示。
    static constexpr std::chrono::milliseconds kFullHintWindow{2};

    // 在 loop 线程上调用，且已 table().TryReserve() 成功。keepalive 让调用方的中间处理器活得比本连接久。
    static ShmConnection* Create(EventLoop& loop,
                                 shm::ShmChannel chan,
                                 PeerInfo peer,
                                 IConnectionHandler& handler,
                                 const ConnectionOptions& options,
                                 std::size_t rx_budget,
                                 std::shared_ptr<ShmConnStats> stats,
                                 std::shared_ptr<void> keepalive);
    ~ShmConnection() override;

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

    // 登记 Rx 的 eventfd、回调 OnConnected，再把 Rx 环里已有的先交出去（App 重启后恢复时里面就有积压）。
    // 登记失败返回 false：只关这一条，没有任何回调。
    [[nodiscard]] bool Establish();

    shm::ShmChannel& channel() noexcept { return chan_; }

private:
    enum class State : std::uint8_t
    {
        Connected,
        Disconnected,
    };

    ShmConnection(EventLoop& loop,
                  shm::ShmChannel chan,
                  PeerInfo peer,
                  IConnectionHandler& handler,
                  std::size_t rx_budget,
                  std::shared_ptr<ShmConnStats> stats,
                  std::shared_ptr<void> keepalive,
                  ConnectionTable::Slot& slot,
                  std::uint32_t slot_index,
                  ConnectionHandle handle);

    void OnEvents(std::uint32_t events) override;
    ConnectionContext* context() const noexcept override { return ctx_.get(); }
    void SendFromTask(SharedBuffer payload) override;
    void CloseFromTask(CloseReason reason) override { CloseWith(reason, 0); }
    void CloseIdle() override { CloseWith(CloseReason::IdleTimeout, 0); }
    void NotifyClosed() override;
    std::uint32_t slot_index() const noexcept override { return slot_index_; }

    SendResult Write(std::span<const std::byte> payload);
    void MarkFull();
    void DrainRx();
    void ScheduleDrain();
    void CloseWith(CloseReason reason, int sys_errno);

    EventLoop& loop_;
    IConnectionHandler& handler_;
    ConnectionTable::Slot& slot_;
    const std::uint32_t slot_index_;
    const std::size_t rx_budget_;
    State state_ = State::Connected;
    bool registered_ = false;
    bool paused_ = false;
    bool notified_connected_ = false;
    bool drain_scheduled_ = false;
    shm::ShmChannel chan_;
    shm::RingProducer tx_;
    shm::RingConsumer rx_;
    PeerInfo peer_;
    const ConnectionHandle handle_;
    std::unique_ptr<ConnectionContext> ctx_;
    CloseReason close_reason_ = CloseReason::LocalClose;
    int close_errno_ = 0;
    std::shared_ptr<ShmConnStats> stats_;
    std::shared_ptr<void> keepalive_;
};

} // namespace breeze::agent
