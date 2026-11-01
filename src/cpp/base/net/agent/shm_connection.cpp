#include "base/net/agent/shm_connection.h"

#include <sys/epoll.h>

#include <cstring>

#include "base/logger/logger.h"
#include "base/logger/logger_macros.h"
#include "base/net/loop/event_loop.h"

namespace breeze::agent
{

ShmConnection* ShmConnection::Create(EventLoop& loop,
                                     shm::ShmChannel chan,
                                     PeerInfo peer,
                                     IConnectionHandler& handler,
                                     const ConnectionOptions& options,
                                     std::size_t rx_budget,
                                     std::shared_ptr<ShmConnStats> stats,
                                     std::shared_ptr<void> keepalive)
{
    loop.AssertInLoopThread();
    ConnectionTable& table = loop.table();
    const std::uint32_t slot = table.Allocate(options.high_water_mark);
    const std::uint32_t generation = table.slot(slot).generation.load(std::memory_order_relaxed);
    const ConnectionHandle handle = loop.MakeHandle(slot, generation);
    std::unique_ptr<ShmConnection> conn(new ShmConnection(loop,
                                                          std::move(chan),
                                                          std::move(peer),
                                                          handler,
                                                          rx_budget,
                                                          std::move(stats),
                                                          std::move(keepalive),
                                                          table.slot(slot),
                                                          slot,
                                                          handle));
    ShmConnection* raw = conn.get();
    table.Install(slot, std::move(conn));
    return raw;
}

ShmConnection::ShmConnection(EventLoop& loop,
                             shm::ShmChannel chan,
                             PeerInfo peer,
                             IConnectionHandler& handler,
                             std::size_t rx_budget,
                             std::shared_ptr<ShmConnStats> stats,
                             std::shared_ptr<void> keepalive,
                             ConnectionTable::Slot& slot,
                             std::uint32_t slot_index,
                             ConnectionHandle handle)
    : loop_(loop)
    , handler_(handler)
    , slot_(slot)
    , slot_index_(slot_index)
    , rx_budget_(rx_budget == 0 ? 1 : rx_budget)
    , chan_(std::move(chan))
    , tx_(chan_.TxProducer())
    , rx_(chan_.RxConsumer())
    , peer_(std::move(peer))
    , handle_(handle)
    , stats_(std::move(stats))
    , keepalive_(std::move(keepalive))
{
}

ShmConnection::~ShmConnection() = default;

bool ShmConnection::Establish()
{
    loop_.AssertInLoopThread();
    if (tx_.corrupt() || rx_.corrupt() || loop_.Add(this, rx_.eventfd(), EPOLLIN) != 0)
    {
        LOG_WARN("shm connection: 建立失败（环的游标不合法或 epoll 登记失败），只关闭这一条");
        state_ = State::Disconnected;
        slot_.state.store(ConnectionTable::State::Disconnected, std::memory_order_release);
        loop_.QueueClose(this); // 没回调过 OnConnected，回收时也就不会有 OnClosed
        return false;
    }
    registered_ = true;
    notified_connected_ = true;
    handler_.OnConnected(*this);
    // 先把环里已有的读一遍：App 重启恢复时里面就有积压；收尾的 PrepareToSleep 才会把睡眠标志置位，Agent 之后才知道要敲 eventfd。
    DrainRx();
    return true;
}

void ShmConnection::OnEvents(std::uint32_t)
{
    if (state_ == State::Disconnected)
        return;
    rx_.WokeUp(); // 清标志、读清 eventfd
    DrainRx();
}

// 读 Rx 环：预算用完要让出（别的连接不被饿死）；暂停时什么都不读，留在环里。
void ShmConnection::DrainRx()
{
    constexpr int kMaxImmediateRounds = 8; // 睡前检查发现又有数据而连续重来的次数上限
    for (int round = 0; round < kMaxImmediateRounds; ++round)
    {
        bool emptied = false;
        for (std::size_t left = rx_budget_; left > 0; --left)
        {
            if (state_ != State::Connected || paused_)
                return;
            shm::RecordView v;
            const shm::PeekStatus st = rx_.Peek(v);
            if (st == shm::PeekStatus::kCorrupt)
            {
                CloseWith(CloseReason::ProtocolError, 0); // 对端写坏了环：断开，不尝试修复
                return;
            }
            if (st == shm::PeekStatus::kEmpty)
            {
                emptied = true;
                break;
            }
            stats_->rx_messages.fetch_add(1, std::memory_order_relaxed);
            handler_.OnMessage(*this, std::span(v.payload, v.len));
            if (state_ != State::Connected)
                return; // 回调里把自己关了
            rx_.Pop();
        }
        if (!emptied)
        {
            ScheduleDrain(); // 预算用完：让出，下一轮再来
            return;
        }
        if (rx_.PrepareToSleep())
            return; // 真的空了：回 epoll 等 eventfd
        // 睡前回头一看又来了：再搬一轮
    }
    ScheduleDrain();
}

void ShmConnection::ScheduleDrain()
{
    if (drain_scheduled_)
        return;
    drain_scheduled_ = true;
    // 任务到的时候连接可能已经回收：靠槽的 generation 识别，不直接捕获 this。
    const std::uint32_t slot = slot_index_;
    const std::uint32_t generation = handle_.generation();
    EventLoop* loop = &loop_;
    loop_.Post(
        [loop, slot, generation]
        {
            auto* c = static_cast<ShmConnection*>(loop->table().Current(slot, generation));
            if (c == nullptr)
                return;
            c->drain_scheduled_ = false;
            c->DrainRx();
        });
}

SendResult ShmConnection::Write(std::span<const std::byte> payload)
{
    if (payload.size() > UINT32_MAX)
    {
        stats_->tx_too_large.fetch_add(1, std::memory_order_relaxed);
        return SendResult::Ok;
    }
    shm::Reserved r = tx_.TryReserve(static_cast<std::uint32_t>(payload.size()));
    switch (r.status)
    {
        case shm::ReserveStatus::kOk:
            if (!payload.empty())
                std::memcpy(r.payload.data(), payload.data(), payload.size());
            tx_.Commit();
            stats_->tx_messages.fetch_add(1, std::memory_order_relaxed);
            return SendResult::Ok;
        case shm::ReserveStatus::kFull:
            stats_->tx_full_drops.fetch_add(1, std::memory_order_relaxed); // Agent 不在或来不及搬：这条丢掉
            MarkFull();
            return SendResult::Full;
        case shm::ReserveStatus::kTooLarge:
            stats_->tx_too_large.fetch_add(1, std::memory_order_relaxed);
            return SendResult::Ok;
        case shm::ReserveStatus::kCorrupt:
            CloseWith(CloseReason::ProtocolError, 0); // Tx 环的 read_idx 被对端写坏了
            return SendResult::Closed;
    }
    return SendResult::Closed;
}

// 置槽的 tx_full 提示并起一个一次性定时器清它：已置位说明定时器在路上，不重复起。
// 定时器到点时连接可能已回收（槽复用）：靠 generation 识别，不捕获 this。
void ShmConnection::MarkFull()
{
    if (slot_.tx_full.exchange(true, std::memory_order_relaxed))
        return;
    const std::uint32_t slot = slot_index_;
    const std::uint32_t generation = handle_.generation();
    EventLoop* loop = &loop_;
    loop_.RunAfter(kFullHintWindow,
                   [loop, slot, generation]
                   {
                       if (loop->table().Current(slot, generation) != nullptr)
                           loop->table().slot(slot).tx_full.store(false, std::memory_order_relaxed);
                   });
}

SendResult ShmConnection::Send(SharedBuffer payload) { return Send(payload.bytes()); }

SendResult ShmConnection::Send(std::span<const std::byte> payload)
{
    loop_.AssertInLoopThread();
    if (state_ != State::Connected)
        return SendResult::Closed;
    return Write(payload);
}

void ShmConnection::SendFromTask(SharedBuffer payload)
{
    if (state_ != State::Connected)
        return; // 连接已标记关闭：槽回收时 pending_bytes 会清零，这里不用减
    const std::uint64_t len = payload.size();
    (void)Write(payload.bytes());
    // 发送方（ConnectionHandle::Send）已经把 len 计入槽的 pending_bytes；写环是同步的，写完（或丢弃）就还回去。
    slot_.pending_bytes.fetch_sub(len, std::memory_order_relaxed);
}

void ShmConnection::Close()
{
    loop_.AssertInLoopThread();
    CloseWith(CloseReason::LocalClose, 0);
}

// 写环不排队，没有"排空发送队列"可等：与 Close 一样。
void ShmConnection::CloseGracefully(std::chrono::milliseconds)
{
    Close();
}

void ShmConnection::PauseRead()
{
    loop_.AssertInLoopThread();
    if (state_ != State::Connected || paused_)
        return;
    paused_ = true;
    if (registered_)
    {
        loop_.Del(rx_.eventfd()); // 不摘的话 eventfd 一直可读（水平触发），空转
        registered_ = false;
    }
}

void ShmConnection::ResumeRead()
{
    loop_.AssertInLoopThread();
    if (!paused_)
        return;
    paused_ = false;
    if (state_ == State::Disconnected)
        return;
    if (loop_.Add(this, rx_.eventfd(), EPOLLIN) != 0)
    {
        CloseWith(CloseReason::IoError, errno);
        return;
    }
    registered_ = true;
    rx_.WokeUp();
    DrainRx(); // 暂停期间攒下的
}

void ShmConnection::CloseWith(CloseReason reason, int sys_errno)
{
    if (state_ == State::Disconnected)
        return;
    state_ = State::Disconnected;
    slot_.state.store(ConnectionTable::State::Disconnected, std::memory_order_release); // 跨线程 Send 从此得到 Closed
    close_reason_ = reason;
    close_errno_ = sys_errno;
    paused_ = false;
    if (registered_)
    {
        loop_.Del(rx_.eventfd());
        registered_ = false;
    }
    loop_.QueueClose(this); // 回收（回调 OnClosed、销毁）在本轮分发之后
}

void ShmConnection::NotifyClosed()
{
    if (!notified_connected_)
        return;
    handler_.OnClosed(*this, close_reason_, close_errno_);
}

} // namespace breeze::agent
