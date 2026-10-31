#include "rudp_connection.h"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <vector>

#include "internal/core/event_loop.h"
#include "logger.h"
#include "logger_macros.h"

namespace breeze
{

namespace
{

// 关闭时给对端发几个 Close：UDP 会丢，三个全丢的概率很小，再多就是浪费；真丢光了对端靠心跳超时。
constexpr int kCloseRepeat = 3;

} // namespace

RudpConnection* RudpConnection::Create(EventLoop& loop,
                                       UdpSocket& socket,
                                       const sockaddr_storage* peer_addr,
                                       socklen_t peer_len,
                                       std::uint32_t conv,
                                       PeerInfo peer,
                                       ConnectionHandler& handler,
                                       Owner& owner,
                                       const ConnectionOptions& conn_options,
                                       const RudpOptions& rudp_options)
{
    loop.AssertInLoopThread();
    // 先建 KCP：失败时还没占槽，调用方只要归还名额。output 的 user 要等连接对象建好才有，下面补。
    std::unique_ptr<KcpSession> kcp = KcpSession::Create(conv, rudp_options, &RudpConnection::KcpOutput, nullptr);
    if (kcp == nullptr)
        return nullptr;
    ConnectionTable& table = loop.table();
    const std::uint32_t slot = table.Allocate(conn_options.high_water_mark);
    const std::uint32_t generation = table.slot(slot).generation.load(std::memory_order_relaxed);
    const ConnectionHandle handle = loop.MakeHandle(slot, generation);
    std::unique_ptr<RudpConnection> conn(new RudpConnection(loop,
                                                            socket,
                                                            peer_addr,
                                                            peer_len,
                                                            conv,
                                                            std::move(kcp),
                                                            std::move(peer),
                                                            handler,
                                                            owner,
                                                            conn_options,
                                                            rudp_options,
                                                            table.slot(slot),
                                                            slot,
                                                            handle));
    RudpConnection* raw = conn.get();
    raw->kcp_->SetUser(raw);
    table.Install(slot, std::move(conn));
    return raw;
}

RudpConnection::RudpConnection(EventLoop& loop,
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
                               ConnectionHandle handle)
    : loop_(loop)
    , socket_(socket)
    , handler_(handler)
    , owner_(owner)
    , kcp_(std::move(kcp))
    , slot_(slot)
    , slot_index_(slot_index)
    , conv_(conv)
    , idle_timeout_ms_(static_cast<std::uint32_t>(
          std::clamp<std::int64_t>(conn_options.idle_timeout.count(), 0, std::numeric_limits<std::uint32_t>::max())))
    , high_water_(conn_options.high_water_mark)
    , max_message_(rudp_options.max_message_size)
    , flush_on_send_(rudp_options.flush_on_send)
    , peer_len_(peer_addr != nullptr ? peer_len : 0)
    , peer_(std::move(peer))
    , handle_(handle)
{
    if (peer_addr != nullptr)
        peer_addr_ = *peer_addr;
}

RudpConnection::~RudpConnection() = default;

void RudpConnection::OnEvents(std::uint32_t) { LOG_FATAL("RudpConnection 没有自己的 fd，不该收到 epoll 事件"); }

void RudpConnection::KcpOutput(void* user, std::span<const std::byte> segment)
{
    auto* self = static_cast<RudpConnection*>(user);
    const std::byte hdr = rudp::DataHeader();
    // 发送失败（缓冲满）按丢包处理，KCP 会重传。
    (void)self->socket_.Send({&hdr, 1}, segment, self->peer_len_ != 0 ? &self->peer_addr_ : nullptr, self->peer_len_);
}

void RudpConnection::SendControl(const rudp::Control& c) noexcept
{
    (void)socket_.Send({}, c.view(), peer_len_ != 0 ? &peer_addr_ : nullptr, peer_len_);
}

void RudpConnection::Establish()
{
    loop_.AssertInLoopThread();
    kcp_->Update(KcpNowMs()); // ikcp 要先 Update 过一次才会 flush
    notified_connected_ = true;
    if (idle_timeout_ms_ != 0)
    {
        loop_.RegisterIdle(slot_index_, idle_timeout_ms_);
        idle_registered_ = true;
    }
    LOG_DEBUG("rudp connection established {} conv={} peer={}",
              handle_.ToString(),
              conv_,
              peer_.endpoint ? peer_.endpoint->ToString() : "-");
    handler_.OnConnected(*this);
}

// ---- 收 ----

void RudpConnection::OnPacket(const rudp::Packet& p)
{
    if (state_ == State::Disconnected)
        return;
    switch (p.type)
    {
        case rudp::PacketType::Data:
        {
            if (kcp_->Input(p.body, KcpNowMs()) < 0)
            {
                LoopStats::Bump(loop_.stats().datagrams_dropped); // conv 不符或段格式不对
                return;
            }
            if (idle_timeout_ms_ != 0)
                loop_.TouchIdle(slot_index_);
            Wake(); // 收到的段要确认，对端的确认可能要释放窗口：下一次 Tick 去 flush
            DeliverReady();
            return;
        }
        case rudp::PacketType::Ping:
        case rudp::PacketType::Pong:
        case rudp::PacketType::Close:
            if (p.conv != conv_)
            {
                LoopStats::Bump(loop_.stats().datagrams_dropped);
                return;
            }
            if (p.type == rudp::PacketType::Close)
            {
                CloseWith(PeerCloseReason(), 0); // 对端的 Close：不回 Close
                return;
            }
            if (idle_timeout_ms_ != 0)
                loop_.TouchIdle(slot_index_);
            if (p.type == rudp::PacketType::Ping)
                SendControl(rudp::EncodePong(conv_));
            return;
        default:
            LoopStats::Bump(loop_.stats().datagrams_dropped); // Hello / Accept / Reject 不该到这里
            return;
    }
}

// 把 KCP 已经收齐的消息交给 handler，直到没有、暂停或连接关闭。Disconnecting 也要交付：
// 优雅关闭期间对端的最后几条消息不能丢。
void RudpConnection::DeliverReady()
{
    // 线程本地、所有会话共用：一次只交付一条，handler 里不会再进入交付路径（ResumeRead 只置标志）。
    static thread_local std::vector<std::byte> buf;
    std::uint64_t messages = 0;
    while (state_ != State::Disconnected && !paused_)
    {
        const int size = kcp_->PeekSize();
        if (size < 0)
            break;
        if (static_cast<std::size_t>(size) > max_message_)
        {
            // 对端的 max_message_size 比我们的大：和 TCP codec 帧超长一样判协议错误。
            LOG_WARN("收到的消息 {} 字节超过 max_message_size={}，关闭连接", size, max_message_);
            CloseWith(CloseReason::ProtocolError, 0);
            break;
        }
        if (buf.size() < static_cast<std::size_t>(size))
            buf.resize(static_cast<std::size_t>(size));
        const int got = kcp_->Recv({buf.data(), static_cast<std::size_t>(size)});
        if (got < 0)
        {
            CloseWith(CloseReason::ProtocolError, 0); // PeekSize 刚说有，Recv 却失败：KCP 内部状态不对
            break;
        }
        ++messages;
        handler_.OnMessage(*this, {buf.data(), static_cast<std::size_t>(got)});
    }
    LoopStats::Bump(loop_.stats().messages_in, messages);
}

// ---- 发 ----

SendResult RudpConnection::Send(SharedBuffer payload) { return SendImpl(payload.bytes(), false); }

SendResult RudpConnection::Send(std::span<const std::byte> payload) { return SendImpl(payload, false); }

void RudpConnection::SendFromTask(SharedBuffer payload)
{
    if (state_ != State::Connected)
        return; // 连接已标记关闭：槽回收时 pending_bytes 会清零，这里不用减
    (void)SendImpl(payload.bytes(), true);
}

SendResult RudpConnection::SendImpl(std::span<const std::byte> payload, bool counted)
{
    loop_.AssertInLoopThread();
    if (state_ != State::Connected)
    {
        LoopStats::Add(loop_.stats().send_closed_stale);
        return SendResult::Closed;
    }
    if (payload.size() > max_message_)
    {
        // 对端按同样的上限会判协议错误：不如本端先关，别让对端带着错位的状态继续跑。
        LOG_ERROR("Send 的 payload 超过 max_message_size={} size={}，关闭连接", max_message_, payload.size());
        LoopStats::Add(loop_.stats().send_closed_oversize);
        CloseWith(CloseReason::ProtocolError, 0);
        return SendResult::Closed;
    }
    // 每条消息先进 KCP 的发送队列，下一次 flush 才进窗口：这段时间算积压（不像 TCP 有"直写成功就不计"的快路径）。
    if (!counted)
        slot_.pending_bytes.fetch_add(payload.size(), std::memory_order_relaxed);
    std::uint32_t fragments = 0;
    if (kcp_->Send(payload, fragments) != 0)
    {
        // max_message_size 已限在 127 段以内（RudpOptions::Valid），走到这里是 KCP 的约束变了。
        LOG_ERROR("KCP 拒绝了 {} 字节的消息，关闭连接", payload.size());
        LoopStats::Add(loop_.stats().send_closed_oversize);
        CloseWith(CloseReason::ProtocolError, 0);
        return SendResult::Closed;
    }
    seg_total_ += fragments;
    queued_.push_back({seg_total_, static_cast<std::uint32_t>(payload.size())});
    Wake(); // 排队的要靠 Tick 发出与重传
    // 排队量涨过水位的一半就立刻 flush 一次，把窗口放得下的移进去，再判水位：一次突发（哪怕大于水位）只要窗口放得下，
    // 就不是慢消费者；窗口真的关上了（对端不收），flush 也移不动，才会越线。小消息仍然靠定时 Update 批量发。
    std::uint64_t pending = slot_.pending_bytes.load(std::memory_order_relaxed);
    if (flush_on_send_ || pending > high_water_ / 2)
    {
        kcp_->Flush();
        ReleaseSent();
        pending = slot_.pending_bytes.load(std::memory_order_relaxed);
    }
    if (!counted && pending > high_water_) // 跨线程来的（counted）已在 ConnectionHandle::Send 里判过
    {
        LOG_WARN("发送积压 {} 字节超过水位 {}，按慢消费者关闭连接", pending, high_water_);
        LoopStats::Add(loop_.stats().send_closed_slow_consumer);
        CloseWith(CloseReason::SlowConsumer, 0);
        return SendResult::Closed;
    }
    LoopStats::Bump(loop_.stats().messages_out);
    return SendResult::Ok;
}

// 已进窗口的段数 = 累计排入 - 还在队列里的。消息的最后一段已进窗口，整条就离开了队列，把它的字节从积压里减掉。
// KCP 只在 flush（Update / Flush）里把队列往窗口里移，所以只需在这两处之后调用；队列先进先出，释放是精确的。
void RudpConnection::ReleaseSent()
{
    if (queued_.empty())
        return;
    const std::uint64_t moved = seg_total_ - kcp_->QueuedSegments();
    while (!queued_.empty() && queued_.front().end_seg <= moved)
    {
        slot_.pending_bytes.fetch_sub(queued_.front().bytes, std::memory_order_relaxed);
        queued_.pop_front();
    }
}

// ---- 定时推进 ----

bool RudpConnection::Tick(std::uint32_t now_ms)
{
    if (state_ == State::Disconnected)
    {
        ticking_ = false;
        return false;
    }
    if (need_deliver_)
    {
        need_deliver_ = false;
        DeliverReady();
        if (state_ == State::Disconnected)
        {
            ticking_ = false;
            return false;
        }
    }
    kcp_->Update(now_ms);
    ReleaseSent();
    if (kcp_->Dead())
    {
        LOG_WARN("rudp 链路判死 {} conv={}", handle_.ToString(), conv_);
        CloseWith(CloseReason::IoError, ETIMEDOUT);
        ticking_ = false;
        return false;
    }
    if (state_ == State::Disconnecting && kcp_->WaitSnd() == 0)
    {
        CloseWith(CloseReason::LocalClose, 0); // 该发的都被确认了：告诉对端我们结束
        ticking_ = false;
        return false;
    }
    // 回调里可能又 Send 了、又来了数据，所以在这里最后判断。静止了就退出活跃表。
    ticking_ = need_deliver_ || kcp_->NeedsUpdate();
    return ticking_;
}

// 从静止变成有事可做：告诉拥有者把我放进活跃表。已经在表里（或已关闭）就什么都不做。
void RudpConnection::Wake()
{
    if (ticking_ || state_ == State::Disconnected)
        return;
    ticking_ = true;
    owner_.OnConnectionWake(*this);
}

// ---- 流控与优雅关闭 ----

void RudpConnection::PauseRead()
{
    loop_.AssertInLoopThread();
    if (state_ != State::Connected || paused_)
        return;
    paused_ = true;
    LoopStats::Bump(loop_.stats().paused);
    if (idle_registered_)
        loop_.SetIdlePaused(slot_index_, true);
}

void RudpConnection::ResumeRead()
{
    loop_.AssertInLoopThread();
    if (!paused_)
        return;
    paused_ = false;
    LoopStats::Drop(loop_.stats().paused);
    if (state_ == State::Disconnected)
        return;
    need_deliver_ = true; // 不在这里直接交付：调用方可能就在 OnMessage 里
    Wake();
    if (idle_registered_)
        loop_.SetIdlePaused(slot_index_, false);
}

void RudpConnection::CloseGracefully(std::chrono::milliseconds timeout)
{
    loop_.AssertInLoopThread();
    if (state_ != State::Connected)
        return;
    state_ = State::Disconnecting;
    slot_.state.store(ConnectionTable::State::Disconnecting, std::memory_order_release); // 跨线程 Send 从此得到 Closed
    if (paused_)
        ResumeRead(); // 暂停着读不到对端在 Close 之前发来的最后几条消息
    grace_timer_ = loop_.RunAfter(timeout,
                                  [this]
                                  {
                                      grace_timer_ = 0;
                                      CloseWith(CloseReason::LocalClose, 0); // 对端没在期限内确认完：强关
                                  });
    // 有待确认的数据时会话一定已经在活跃表里（Send 唤醒它，Tick 在 NeedsUpdate 时不退出），靠 Tick 等确认；
    // 没有待确认的就不用等了。
    if (kcp_->WaitSnd() == 0)
        CloseWith(CloseReason::LocalClose, 0);
}

// ---- 关闭：标记 → 本轮分发之后回调 OnClosed → 销毁 ----

void RudpConnection::Close() { CloseWith(CloseReason::LocalClose, 0); }

void RudpConnection::CloseWith(CloseReason reason, int sys_errno)
{
    if (state_ == State::Disconnected)
        return;
    state_ = State::Disconnected;
    slot_.state.store(ConnectionTable::State::Disconnected, std::memory_order_release); // 跨线程 Send 从此得到 Closed
    close_reason_ = reason;
    close_errno_ = sys_errno;
    // 下面三件都要在回收之前做完：定时器里捕获着 this；暂停数与空闲登记是 loop 级的计数。
    if (grace_timer_ != 0)
    {
        loop_.Cancel(grace_timer_);
        grace_timer_ = 0;
    }
    if (paused_)
    {
        paused_ = false;
        LoopStats::Drop(loop_.stats().paused);
    }
    if (idle_registered_)
    {
        loop_.UnregisterIdle(slot_index_);
        idle_registered_ = false;
    }
    // 对端发来的 Close 引起的关闭不回；链路判死时对端大概率已经不可达，也不发。
    if (reason != CloseReason::PeerClosed && reason != CloseReason::IoError)
    {
        const rudp::Control c = rudp::EncodeClose(conv_, static_cast<std::uint8_t>(reason));
        for (int i = 0; i < kCloseRepeat; ++i)
            SendControl(c);
    }
    loop_.QueueClose(this);
}

void RudpConnection::NotifyClosed()
{
    owner_.OnConnectionDetached(*this); // 先摘表：OnClosed 里拥有者就看不到这个会话了
    if (!notified_connected_)
        return;
    LoopStats::Bump(loop_.stats().closed[static_cast<std::size_t>(close_reason_)]);
    LOG_DEBUG(
        "rudp connection closed {} reason={} errno={}", handle_.ToString(), ToString(close_reason_), close_errno_);
    handler_.OnClosed(*this, close_reason_, close_errno_);
}

} // namespace breeze
